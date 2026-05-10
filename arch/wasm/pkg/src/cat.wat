;; cat.wat — reads stdin to EOF, writes to stdout. POSIX cat with no
;; filename support (kernel feeds the file via stdin redirection or
;; pipe). Buffer is 4 KiB.
(module
  (import "wasi_snapshot_preview1" "fd_read"
    (func $fd_read (param i32 i32 i32 i32) (result i32)))
  (import "wasi_snapshot_preview1" "fd_write"
    (func $fd_write (param i32 i32 i32 i32) (result i32)))
  (import "wasi_snapshot_preview1" "proc_exit"
    (func $proc_exit (param i32)))
  (memory (export "memory") 1)

  ;; Layout: iov at 0 (8 bytes), nread/nwrote at 8/12, buffer at 4096.
  (func $_start (export "_start")
    (local $n i32)
    (block $eof (loop $L
      ;; iov.buf=4096, iov.len=4096
      (i32.store (i32.const 0) (i32.const 4096))
      (i32.store (i32.const 4) (i32.const 4096))
      (drop (call $fd_read (i32.const 0) (i32.const 0) (i32.const 1) (i32.const 8)))
      (local.set $n (i32.load (i32.const 8)))
      (br_if $eof (i32.eqz (local.get $n)))
      ;; iov.buf=4096, iov.len=n
      (i32.store (i32.const 0) (i32.const 4096))
      (i32.store (i32.const 4) (local.get $n))
      (drop (call $fd_write (i32.const 1) (i32.const 0) (i32.const 1) (i32.const 12)))
      (br $L)))
    (call $proc_exit (i32.const 0))
  )
)
