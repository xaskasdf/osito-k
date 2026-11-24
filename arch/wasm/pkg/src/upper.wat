;; upper.wat — translate ASCII a-z to A-Z from stdin to stdout.
;; Simpler than full POSIX tr — just one fixed transform.
(module
  (import "wasi_snapshot_preview1" "fd_read"
    (func $fd_read (param i32 i32 i32 i32) (result i32)))
  (import "wasi_snapshot_preview1" "fd_write"
    (func $fd_write (param i32 i32 i32 i32) (result i32)))
  (import "wasi_snapshot_preview1" "proc_exit"
    (func $proc_exit (param i32)))
  (memory (export "memory") 1)

  (func $_start (export "_start")
    (local $n i32) (local $i i32) (local $c i32)
    (block $eof (loop $L
      (i32.store (i32.const 0) (i32.const 4096))
      (i32.store (i32.const 4) (i32.const 4096))
      (drop (call $fd_read (i32.const 0) (i32.const 0) (i32.const 1) (i32.const 8)))
      (local.set $n (i32.load (i32.const 8)))
      (br_if $eof (i32.eqz (local.get $n)))
      (local.set $i (i32.const 0))
      (block $cd (loop $C
        (br_if $cd (i32.ge_s (local.get $i) (local.get $n)))
        (local.set $c (i32.load8_u (i32.add (i32.const 4096) (local.get $i))))
        (if (i32.and
              (i32.ge_u (local.get $c) (i32.const 97))
              (i32.le_u (local.get $c) (i32.const 122)))
          (then
            (i32.store8 (i32.add (i32.const 4096) (local.get $i))
              (i32.sub (local.get $c) (i32.const 32)))))
        (local.set $i (i32.add (local.get $i) (i32.const 1)))
        (br $C)))
      (i32.store (i32.const 0) (i32.const 4096))
      (i32.store (i32.const 4) (local.get $n))
      (drop (call $fd_write (i32.const 1) (i32.const 0) (i32.const 1) (i32.const 12)))
      (br $L)))
    (call $proc_exit (i32.const 0))
  )
)
