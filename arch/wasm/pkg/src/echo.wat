;; echo.wat — minimal WASI program that reads argv via args_get and
;; writes each argument on its own line. Demonstrates argv handling.
(module
  (import "wasi_snapshot_preview1" "fd_write"
    (func $fd_write (param i32 i32 i32 i32) (result i32)))
  (import "wasi_snapshot_preview1" "args_sizes_get"
    (func $args_sizes_get (param i32 i32) (result i32)))
  (import "wasi_snapshot_preview1" "args_get"
    (func $args_get (param i32 i32) (result i32)))
  (import "wasi_snapshot_preview1" "proc_exit"
    (func $proc_exit (param i32)))
  (memory (export "memory") 1)

  (func $strlen (param $p i32) (result i32)
    (local $n i32)
    (local.set $n (i32.const 0))
    (block $d (loop $L
      (br_if $d (i32.eqz
        (i32.load8_u (i32.add (local.get $p) (local.get $n)))))
      (local.set $n (i32.add (local.get $n) (i32.const 1)))
      (br $L)))
    (local.get $n))

  ;; write_str(p): write null-terminated string + '\n' to stdout.
  (func $write_str (param $p i32)
    (local $len i32)
    (local.set $len (call $strlen (local.get $p)))
    ;; Write the string itself.
    (i32.store (i32.const 0) (local.get $p))
    (i32.store (i32.const 4) (local.get $len))
    (drop (call $fd_write (i32.const 1) (i32.const 0) (i32.const 1) (i32.const 8)))
    ;; Write '\n'.
    (i32.store8 (i32.const 12) (i32.const 10))
    (i32.store (i32.const 0) (i32.const 12))
    (i32.store (i32.const 4) (i32.const 1))
    (drop (call $fd_write (i32.const 1) (i32.const 0) (i32.const 1) (i32.const 8))))

  (func $_start (export "_start")
    (local $argc i32) (local $i i32) (local $argv i32) (local $p i32)
    ;; args_sizes_get(&argc, &argv_buf_size) → 16, 20
    (drop (call $args_sizes_get (i32.const 16) (i32.const 20)))
    (local.set $argc (i32.load (i32.const 16)))
    ;; argv array starts at 64 (16 ptrs of 4 bytes), strings after.
    (local.set $argv (i32.const 64))
    ;; args_get(argv_ptrs, argv_buf=128)
    (drop (call $args_get (local.get $argv) (i32.const 128)))
    ;; Skip argv[0] (program name), print argv[1..]
    (local.set $i (i32.const 1))
    (block $d (loop $L
      (br_if $d (i32.ge_s (local.get $i) (local.get $argc)))
      (local.set $p
        (i32.load
          (i32.add (local.get $argv)
            (i32.mul (local.get $i) (i32.const 4)))))
      (call $write_str (local.get $p))
      (local.set $i (i32.add (local.get $i) (i32.const 1)))
      (br $L)))
    (call $proc_exit (i32.const 0))
  )
)
