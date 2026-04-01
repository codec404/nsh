# nsh

A Unix shell written in C, inspired by [Nushell](https://www.nushell.sh). Commands produce **typed tables** instead of raw text, making filtering, sorting, and querying first-class operations. Also supports full scripting with functions, loops, and conditionals.

## Quick start

```sh
# Dependencies (macOS)
brew install readline sqlite

make
./build/nsh
```

Binary is placed at `build/nsh`. Run a script directly with `./build/nsh script.nsh`.

---

## Prompt

```
user:~/projects (main) [2j] $
```

Shows: username · current directory · git branch · background job count · shellenv depth · exit status colour.

---

## Table pipelines

Commands return typed tables — pipe them into filters and queries:

```sh
ls | sort-by size
ls | where size > 10kb | select name size
ls | where name =~ "\.c$" | sort-by modified

history --failed --since 2d
history | where duration_ms > 5000 | sort-by duration_ms
sessions

env | where name =~ "PATH"
```

---

## Scripting

### Variables

```sh
set name "Alice"
echo "Hello, $name!"
unset name
```

### Functions

```sh
def greet name {
    echo "Hello, $name!"
}
greet World
```

### If / else

```sh
if test $x -gt 5 {
    echo "big"
} else {
    echo "small"
}
```

### For loop

```sh
for f in *.c {
    echo "file: $f"
}
```

### While loop

```sh
set i 0
while test $i -lt 5 {
    echo $i
    set i $(expr $i + 1)
}
```

### Break / continue / return

```sh
for x in 1 2 3 4 5 {
    if test $x -eq 3 { break }
    echo $x
}

def first_even nums {
    for n in $nums {
        if test $(expr $n % 2) -eq 0 { return $n }
    }
}
```

### Command substitution

```sh
set today $(date +%Y-%m-%d)
echo "Today is $today"
```

### Multi-line REPL input

Opening a `{` in the REPL without closing it enters continuation mode:

```
nsh> def add a b {
...>     expr $a + $b
... > }
```

---

## History

```sh
history                     # last 100 commands
history --failed            # only failed commands
history --since 1h          # last hour
history --session           # this session only
history --touched ./file    # commands run in same directory
sessions                    # list all sessions
replay                      # interactively replay current session
replay --session 12345      # replay a past session
replay --dry-run            # preview without running
```

---

## Per-directory environment (`.shellenv`)

Place a `.shellenv` file in any directory:

```ini
[env]
NODE_ENV=development
PATH=$PATH:/project/bin

[on_enter]
echo "Project env loaded"

[on_exit]
echo "Leaving project"
```

nsh auto-loads it on `cd` and unloads it when you leave.

```sh
shellenv                    # show active env stack
shellenv diff ./other-dir   # preview what would change
shellenv link               # create template in cwd
shellenv reload             # force reload
```

---

## Config (`~/.config/nsh/config.toml`)

```toml
[prompt]
format           = "%u:%w%g%j%e%s "
show_duration_ms = 500

[history]
size = 1000
```

Prompt tokens: `%u` user · `%w` cwd · `%g` git branch · `%j` job count · `%e` shellenv depth · `%s` dollar sign.

---

## Init file (`~/.nshrc`)

Loaded on every interactive startup. Use it for aliases and functions:

```sh
def ll { ls -la $1 }
def gs { git status }
set EDITOR vim
```

---

## Builtins

| Command | Description |
|---------|-------------|
| `cd [dir]` | Change directory (`-` for previous) |
| `jobs` | List background jobs |
| `fg [n]` | Bring job to foreground |
| `bg [n]` | Resume job in background |
| `set var val` | Set environment variable |
| `unset var` | Unset environment variable |
| `break` | Break out of a loop |
| `continue` | Skip to next loop iteration |
| `return [n]` | Return from function with exit code |
| `exit [n]` | Exit the shell |
| `history` | Query command history |
| `sessions` | List shell sessions |
| `replay` | Replay session commands |
| `shellenv` | Manage per-directory environments |
| `where` | Filter table rows |
| `select` | Select table columns |
| `sort-by` | Sort table by column |

---

## Keyboard shortcuts

| Key | Action |
|-----|--------|
| `Tab` | Complete commands, columns, or filenames |
| `Ctrl+C` | Cancel current input |
| `Ctrl+L` | Clear screen and scrollback |
| `Ctrl+Z` | Suspend foreground job |
| `↑ / ↓` | Navigate history |

---

## Shell features

```sh
echo "hello $USER"          # variable expansion
echo $(date)                # command substitution
cat file.txt | grep foo     # pipes
ls > out.txt                # output redirection
ls >> out.txt               # append redirection
cat < file.txt              # input redirection
sleep 10 &                  # background jobs
fg, bg, jobs                # job control
cd -                        # previous directory
```
