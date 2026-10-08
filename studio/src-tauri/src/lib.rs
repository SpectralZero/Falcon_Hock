// Hexforge Studio — Tauri bridge.
//
// Owns a client connection to the umf_runtime IPC server, a newline-delimited
// JSON-RPC 2.0 channel over the per-process named pipe \\.\pipe\umf-studio-<pid>.
// A dedicated reader thread routes responses to their waiting callers by id and
// forwards `log` notifications to the frontend as `umf://log` events.

use std::collections::HashMap;
use std::fs::{File, OpenOptions};
use std::io::{BufRead, BufReader, Write};
use std::sync::atomic::{AtomicBool, AtomicU64, Ordering};
use std::sync::mpsc::{channel, Sender};
use std::sync::{Arc, Mutex};
use std::time::Duration;

use serde::Serialize;
use serde_json::{json, Value};
use tauri::{AppHandle, Emitter, State};

const RPC_TIMEOUT: Duration = Duration::from_secs(5);

struct Conn {
    writer: Mutex<File>,
    pending: Mutex<HashMap<u64, Sender<Value>>>,
    next_id: AtomicU64,
    stop: Arc<AtomicBool>,
}

#[derive(Default)]
struct IpcState {
    conn: Mutex<Option<Arc<Conn>>>,
}

#[derive(Serialize)]
struct ConnectInfo {
    pid: u32,
    pipe: String,
}

#[tauri::command]
fn umf_connect(app: AppHandle, state: State<IpcState>, pid: u32) -> Result<ConnectInfo, String> {
    // Retire any previous connection first.
    if let Some(old) = state.conn.lock().unwrap().take() {
        old.stop.store(true, Ordering::SeqCst);
    }

    let pipe = format!(r"\\.\pipe\umf-studio-{pid}");
    let file = OpenOptions::new()
        .read(true)
        .write(true)
        .open(&pipe)
        .map_err(|e| format!("cannot open {pipe}: {e}"))?;
    let reader_file = file.try_clone().map_err(|e| format!("clone pipe handle: {e}"))?;

    let stop = Arc::new(AtomicBool::new(false));
    let conn = Arc::new(Conn {
        writer: Mutex::new(file),
        pending: Mutex::new(HashMap::new()),
        next_id: AtomicU64::new(1),
        stop: stop.clone(),
    });

    // Reader thread: route responses by id, forward log notifications.
    let conn_r = conn.clone();
    let app_r = app.clone();
    std::thread::spawn(move || {
        let mut reader = BufReader::new(reader_file);
        let mut line = String::new();
        loop {
            if conn_r.stop.load(Ordering::SeqCst) {
                break;
            }
            line.clear();
            match reader.read_line(&mut line) {
                Ok(0) => break, // EOF: server closed
                Ok(_) => {
                    let trimmed = line.trim();
                    if trimmed.is_empty() {
                        continue;
                    }
                    if let Ok(v) = serde_json::from_str::<Value>(trimmed) {
                        let is_response =
                            v.get("id").is_some() && (v.get("result").is_some() || v.get("error").is_some());
                        if is_response {
                            if let Some(id) = v.get("id").and_then(|x| x.as_u64()) {
                                if let Some(tx) = conn_r.pending.lock().unwrap().remove(&id) {
                                    let _ = tx.send(v);
                                }
                            }
                        } else if v.get("method").and_then(|m| m.as_str()) == Some("log") {
                            let params = v.get("params").cloned().unwrap_or(Value::Null);
                            let _ = app_r.emit("umf://log", params);
                        }
                    }
                }
                Err(_) => break,
            }
        }
        let _ = app_r.emit("umf://disconnect", ());
    });

    *state.conn.lock().unwrap() = Some(conn);
    Ok(ConnectInfo { pid, pipe })
}

#[tauri::command]
fn umf_disconnect(state: State<IpcState>) {
    if let Some(conn) = state.conn.lock().unwrap().take() {
        conn.stop.store(true, Ordering::SeqCst);
    }
}

#[tauri::command]
fn umf_rpc(state: State<IpcState>, method: String, params: Option<Value>) -> Result<Value, String> {
    let conn = {
        let guard = state.conn.lock().unwrap();
        guard.clone().ok_or_else(|| "not connected".to_string())?
    };

    let id = conn.next_id.fetch_add(1, Ordering::SeqCst);
    let (tx, rx) = channel::<Value>();
    conn.pending.lock().unwrap().insert(id, tx);

    let req = json!({
        "jsonrpc": "2.0",
        "id": id,
        "method": method,
        "params": params.unwrap_or(Value::Null),
    });
    let line = format!("{req}\n");

    {
        let mut w = conn.writer.lock().unwrap();
        if let Err(e) = w.write_all(line.as_bytes()) {
            conn.pending.lock().unwrap().remove(&id);
            return Err(format!("write failed: {e}"));
        }
        let _ = w.flush();
    }

    match rx.recv_timeout(RPC_TIMEOUT) {
        Ok(v) => {
            if let Some(err) = v.get("error") {
                let msg = err
                    .get("message")
                    .and_then(|m| m.as_str())
                    .unwrap_or("rpc error");
                Err(msg.to_string())
            } else {
                Ok(v.get("result").cloned().unwrap_or(Value::Null))
            }
        }
        Err(_) => {
            conn.pending.lock().unwrap().remove(&id);
            Err("rpc timeout".to_string())
        }
    }
}

#[cfg_attr(mobile, tauri::mobile_entry_point)]
pub fn run() {
    tauri::Builder::default()
        .plugin(tauri_plugin_opener::init())
        .manage(IpcState::default())
        .invoke_handler(tauri::generate_handler![umf_connect, umf_disconnect, umf_rpc])
        .run(tauri::generate_context!())
        .expect("error while running tauri application");
}
