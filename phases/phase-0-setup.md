# Phase 0 — Setup (Bite 0)

You only pass through here once, but a bad setup poisons every later bite.
Do not skip parts; do not "come back to it later."

---

## Bite 0 — Forge and Anvil

> **Goal:** A working C development environment on Windows, where all 80 bites will run.
>
> **Learn:**
> - Why WSL2: sockets, `epoll`, `pthread`, `valgrind`, and every WebRTC library assume POSIX/Linux. Doing Phase 5+ natively on Windows means re-learning everything later.
> - The toolchain pieces: `gcc` (compiler), `gdb` (debugger), `make`/`cmake` (build), `valgrind` (memory checker), `pkg-config` (library finder).
>
> **Do:**
> 1. On Windows PowerShell (admin): `wsl --install` → reboot → create your Ubuntu user.
> 2. Inside Ubuntu:
>    ```
>    sudo apt update && sudo apt install -y build-essential gdb valgrind cmake git pkg-config
>    gcc --version && gdb --version && valgrind --version
>    ```
> 3. Choose an editor: VS Code with the *C/C++* and *WSL* extensions (open remote WSL folders), or Vim/Neovim inside WSL.
> 4. Create the scaffold:
>    ```
>    mkdir -p ~/c-prep/bites && cd ~/c-prep && git init
>    echo 'build/\n*.o\na.out\n' > .gitignore
>    ```
> 5. Mirror this plan into the repo: copy `PLAN.md` and `PROGRESS.md` in.
> 6. Write and run hello world **from the terminal only** (no IDE magic yet):
>    ```c
>    #include <stdio.h>
>    int main(void) { printf("hello, elephant\n"); return 0; }
>    ```
>    Compile: `gcc -Wall -Wextra -Werror -std=c17 -g hello.c -o hello && ./hello`
>
> **Break it on purpose:** Remove the semicolon. Remove `#include <stdio.h>`. Run each and read the compiler's complaint out loud. These are the two errors you'll see most this year.
>
> **Done when:** `./hello` prints the message, `gcc --version` works, the repo has its first commit, and you can explain in one sentence what `-Wall -Wextra -Werror` does.
