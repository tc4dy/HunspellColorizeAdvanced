![I Love Torvalds](ilovetorvalds.png)

# WE LOVE TORVALDS!

## Stop-gap "Help Linus with typos" tool
My google-fu failed me, and I couldn't find a simple colorizing pager
that just dealt with basic spelling issues.
This is very much a stop-gap, because the real solution is to teach
uemacs to do it, but I haven't touched that source tree in years and I
wanted to test hunspell on something simpler first.
This is about as simple as it gets, without being _so_ simple that it is
useless.  I can do
    export LESS=-FRSX
    export GIT_PAGER=huncolor
and the result is usable, and works reasonably well together with the
existing git colorization.
And no, this does no context-aware coloring at all.  Pathnames, URLs,
this silly thing doesn't recognize any of that, just looks at things
that might be words.  In US ASCII only. What a crock.

---

## Changes from original

**Cache**

[+] Hash function replaced with FNV-1a for significantly better distribution
    and fewer collisions under real workloads.
[+] Direct-mapped cache upgraded to 2-way associative (CACHE_WAYS=2);
    collision rate roughly halved with no increase in memory footprint.
[+] Removed time_t stamp from cache entries; replaced with a plain int
    occupied flag — eliminates a time(NULL) call per cache store.

**I/O**

[+] Output buffering layer added (8 KB outbuf); multiple write(2) syscalls
    per word are now batched into a single flush, reducing syscall overhead
    significantly on large inputs.
[+] ANSI escape sequence lengths computed at compile time via sizeof()
    instead of strlen() at runtime on every misspelled word.

**Correctness & safety**

[+] realloc() calls wrapped in xrealloc(); original pointer is never
    overwritten before checking the return value — eliminates a classic
    memory leak on allocation failure.
[+] dup2(2) return values checked in both parent and child paths.
[+] sigaction(2) return values checked; failures logged as warnings.
[+] localtime(3) replaced with thread-safe localtime_r(3).
[+] Child process calls Hunspell_destroy() before exec_pager() to avoid
    leaking file descriptors across exec.
[+] Unused config fields case_sensitive and show_suggestions removed.

**Structure**

[+] process_buffer() decomposed into focused single-purpose functions:
    process_letter(), process_alnum_or_underscore(), process_multibyte(),
    process_control().
[+] main() decomposed into setup_pipe(), spawn_pager(), init_state(),
    run_loop(), cleanup_config().
[+] parser_state_t extracted as a named enum, no longer anonymous inside
    state_t.
[+] Pager argument management centralized in pager_args_append() and
    pager_args_append_tokenized(); eliminates repeated realloc patterns.
[+] Magic numbers replaced with named constants (OUTBUF_SIZE,
    CACHE_WAYS, MAX_DEFAULT_PAGER_ARGS, ANSI_BOLD_ON_LEN, etc.).
[+] Raw file descriptors 0/1 replaced with STDIN_FILENO/STDOUT_FILENO.

**Build**

[+] Makefile rewritten with automatic header dependency tracking (-MMD -MP).
[+] Security hardening flags added: -fstack-protector-strong,
    -D_FORTIFY_SOURCE=2, -Wl,-z,relro, -Wl,-z,now.
[+] Extended warning set: -Wextra -Wpedantic -Wformat=2 -Wformat-security
    -Wnull-dereference -Wshadow.
[+] debug target added with -fsanitize=address,undefined for development.
[+] strip and uninstall targets added.
[+] PREFIX defaults to $(HOME)/.local; no sudo required for installation.

