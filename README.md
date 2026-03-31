# nsh

A Unix shell written in C, inspired by [Nushell](https://www.nushell.sh). Commands produce **typed tables** instead of raw text, making filtering, sorting, and querying first-class operations.

## Quick start

```sh
# Dependencies (macOS)
brew install readline sqlite

make
./nsh
```

## What it looks like

```
user:~/projects (main) $
```
Prompt shows: username, current directory, git branch, background job count, shellenv depth, exit status colour.

## Table pipelines

```sh
ls | sort-by size
ls | where size > 10kb | select name size
ls | where name =~ "\.c$" | sort-by modified
history --failed --since 2d
history | where duration_ms > 5000 | sort-by duration_ms
sessions
env | where name =~ "PATH"
```

## Shell features

```sh
echo "hello $USER"          # variable expansion
echo $(date)                # command substitution
cat file.txt | grep foo     # pipes to external commands
ls > out.txt                # redirection
sleep 10 &                  # background jobs
fg, bg, jobs                # job control
cd -                        # previous directory
```

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

## Per-directory environment (.shellenv)

```ini
[env]
NODE_ENV=development
PATH=$PATH:/project/bin

[on_enter]
echo "Project env loaded"

[on_exit]
echo "Leaving project"
```

```sh
shellenv                    # show active env stack
shellenv diff ./other-dir   # preview what would change
shellenv link               # create template in cwd
shellenv reload             # force reload
```

## Config (`~/.config/nsh/config.toml`)

```toml
[prompt]
format           = "%u:%w%g%j%e%s "
show_duration_ms = 500

[history]
size = 1000
```

Prompt tokens: `%u` user, `%w` cwd, `%g` git branch, `%j` job count, `%e` shellenv depth, `%s` dollar sign.

## Tab completion & prediction

- `Tab` — completes commands from PATH, column names after `where`/`select`/`sort-by`, filenames elsewhere
- `→` / `Ctrl+F` — accept full inline prediction from history
- `Alt+→` / `Alt+F` — accept one word of the prediction
