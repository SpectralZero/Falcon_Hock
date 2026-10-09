/*
 * practice.c — a tiny, safe target to learn the Hexforge scanner on.
 *
 * It keeps two values in memory (health, ammo) and prints their live values
 * AND their addresses, so after you scan you can confirm the tool found the
 * exact same address. Change the values with the keyboard and watch the
 * scanner narrow down to them.
 */
#include <stdio.h>
#include <windows.h>
#include <conio.h>

volatile int g_health = 1000;
volatile int g_ammo   = 50;

int main(void) {
    printf("================================================\n");
    printf("  Hexforge practice target\n");
    printf("  PID: %lu\n", (unsigned long)GetCurrentProcessId());
    printf("================================================\n");
    printf("  Keys:  [-] damage 10   [+] heal 10\n");
    printf("         [a] use ammo    [r] reload ammo\n");
    printf("         [q] quit\n\n");

    for (;;) {
        printf("\r  health = %-6d (@ %p)    ammo = %-4d (@ %p)        ",
               g_health, (void*)&g_health, g_ammo, (void*)&g_ammo);
        fflush(stdout);

        if (_kbhit()) {
            int c = _getch();
            if (c == '-') g_health -= 10;
            else if (c == '+') g_health += 10;
            else if (c == 'a') g_ammo -= 1;
            else if (c == 'r') g_ammo = 50;
            else if (c == 'q') break;
        }
        Sleep(50);
    }
    printf("\n\nbye\n");
    return 0;
}
