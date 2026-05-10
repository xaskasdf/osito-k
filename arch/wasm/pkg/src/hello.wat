;; hello.wat — minimal WASI program.
;; writes "hello, osito\n" to stdout (fd=1) via fd_write, exits 0.
(module
  (import "wasi_snapshot_preview1" "fd_write"
    (func $fd_write (param i32 i32 i32 i32) (result i32)))
  (import "wasi_snapshot_preview1" "proc_exit"
    (func $proc_exit (param i32)))
  (memory (export "memory") 1)

  ;; Layout in linear memory:
  ;;   0x00  iov: i32 buf_ptr, i32 buf_len
  ;;   0x10  string "hello, osito\n"
  (data (i32.const 16) "hello, osito\n")

  (func $_start (export "_start")
    ;; iov.buf = 16, iov.len = 13
    (i32.store (i32.const 0)  (i32.const 16))
    (i32.store (i32.const 4)  (i32.const 13))
    ;; fd_write(fd=1, iovs=0, iovs_len=1, &nwritten=8)
    (drop (call $fd_write (i32.const 1) (i32.const 0) (i32.const 1) (i32.const 8)))
    (call $proc_exit (i32.const 0))
  )
)
