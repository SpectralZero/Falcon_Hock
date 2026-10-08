import type { ReactNode } from "react";

export function Card(props: {
  icon?: ReactNode;
  title?: ReactNode;
  actions?: ReactNode;
  children: ReactNode;
  className?: string;
}) {
  const { icon, title, actions, children, className } = props;
  const hasHead = icon || title || actions;
  return (
    <div className={"card " + (className ?? "")}>
      {hasHead && (
        <div className="card-head">
          {icon && <span className="ch-icon">{icon}</span>}
          {title && <span className="card-title">{title}</span>}
          <span className="spacer" />
          {actions}
        </div>
      )}
      <div className="card-body">{children}</div>
    </div>
  );
}

export function Badge({ tone = "muted", children }: { tone?: string; children: ReactNode }) {
  return <span className={"badge " + tone}>{children}</span>;
}

export function EmptyState({ icon, title, hint }: { icon?: ReactNode; title: string; hint?: string }) {
  return (
    <div className="empty">
      {icon}
      <div style={{ fontWeight: 600, color: "var(--txt-dim)" }}>{title}</div>
      {hint && <div style={{ fontSize: 12 }}>{hint}</div>}
    </div>
  );
}

export function StatRow({ k, v }: { k: ReactNode; v: ReactNode }) {
  return (
    <div className="stat-row">
      <span className="k">{k}</span>
      <span className="v">{v}</span>
    </div>
  );
}
