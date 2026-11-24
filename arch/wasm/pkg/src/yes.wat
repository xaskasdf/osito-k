;; yes.wat — print "y\n" forever. POSIX yes (with default literal "y").
;; Useful as a sanity test for stream pipelines and SIGPIPE behavior.
(module
  (import "wasi_snapshot_preview1" "fd_write"
    (func $fd_write (param i32 i32 i32 i32) (result i32)))
  (import "wasi_snapshot_preview1" "proc_exit"
    (func $proc_exit (param i32)))
  (memory (export "memory") 1)

  (data (i32.const 16) "y\n")

  (func $_start (export "_start")
    (local $rc i32)
    (i32.store (i32.const 0) (i32.const 16))
    (i32.store (i32.const 4) (i32.const 2))
    (block $broken (loop $L
      (local.set $rc
        (call $fd_write (i32.const 1) (i32.const 0) (i32.const 1) (i32.const 8)))
      (br_if $broken (local.get $rc))   ;; non-zero errno -> stop
      (br $L)))
    (call $proc_exit (i32.const 0))
  )
)
