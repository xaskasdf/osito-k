;; grep.wat — naive substring grep over stdin. Reads up to 4 KiB
;; lines, emits each line whose contents contain argv[1] verbatim.
;; No regex; no flags. Layout:
;;   0..7   iov struct
;;   8..15  fd_read nread / fd_write nwritten
;;   64     argv pointer table (16 ptrs)
;;   128    argv buffer
;;   1024   needle bytes (copied from argv[1])
;;   2048   needle length
;;   4096   line buffer (4 KiB)
(module
  (import "wasi_snapshot_preview1" "fd_read"
    (func $fd_read (param i32 i32 i32 i32) (result i32)))
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
    (block $d (loop $L
      (br_if $d (i32.eqz
        (i32.load8_u (i32.add (local.get $p) (local.get $n)))))
      (local.set $n (i32.add (local.get $n) (i32.const 1)))
      (br $L)))
    (local.get $n))

  ;; line_contains: does buf[off..off+len) contain needle bytes at $needle of $nlen?
  (func $line_contains (param $off i32) (param $len i32)
                       (param $needle i32) (param $nlen i32) (result i32)
    (local $i i32) (local $j i32) (local $ok i32)
    (if (i32.eqz (local.get $nlen)) (then (return (i32.const 1))))
    (if (i32.gt_s (local.get $nlen) (local.get $len))
      (then (return (i32.const 0))))
    (local.set $i (i32.const 0))
    (block $d (loop $L
      (br_if $d (i32.gt_s
        (i32.add (local.get $i) (local.get $nlen)) (local.get $len)))
      (local.set $ok (i32.const 1))
      (local.set $j (i32.const 0))
      (block $e (loop $K
        (br_if $e (i32.ge_s (local.get $j) (local.get $nlen)))
        (if (i32.ne
              (i32.load8_u (i32.add (i32.add (i32.const 4096) (local.get $off))
                                    (i32.add (local.get $i) (local.get $j))))
              (i32.load8_u (i32.add (local.get $needle) (local.get $j))))
          (then (local.set $ok (i32.const 0)) (br $e)))
        (local.set $j (i32.add (local.get $j) (i32.const 1)))
        (br $K)))
      (if (local.get $ok) (then (return (i32.const 1))))
      (local.set $i (i32.add (local.get $i) (i32.const 1)))
      (br $L)))
    (i32.const 0))

  (func $_start (export "_start")
    (local $argc i32) (local $needle i32) (local $nlen i32)
    (local $n i32) (local $i i32) (local $line_start i32)
    ;; Read argv. We need argv[1] as the needle.
    (drop (call $args_sizes_get (i32.const 16) (i32.const 20)))
    (local.set $argc (i32.load (i32.const 16)))
    (drop (call $args_get (i32.const 64) (i32.const 128)))
    (if (i32.lt_s (local.get $argc) (i32.const 2))
      (then (call $proc_exit (i32.const 1))))
    (local.set $needle (i32.load (i32.add (i32.const 64) (i32.const 4))))
    (local.set $nlen (call $strlen (local.get $needle)))

    ;; Stream stdin in 4 KiB chunks. We assume each chunk ends at or after
    ;; a newline so we don't have to maintain a leftover prefix across reads.
    (block $eof (loop $L
      (i32.store (i32.const 0) (i32.const 4096))
      (i32.store (i32.const 4) (i32.const 4096))
      (drop (call $fd_read (i32.const 0) (i32.const 0) (i32.const 1) (i32.const 8)))
      (local.set $n (i32.load (i32.const 8)))
      (br_if $eof (i32.eqz (local.get $n)))
      (local.set $line_start (i32.const 0))
      (local.set $i (i32.const 0))
      (block $cd (loop $C
        (br_if $cd (i32.ge_s (local.get $i) (local.get $n)))
        (if (i32.eq
              (i32.load8_u (i32.add (i32.const 4096) (local.get $i)))
              (i32.const 10))
          (then
            (if (call $line_contains
                  (local.get $line_start)
                  (i32.sub (local.get $i) (local.get $line_start))
                  (local.get $needle) (local.get $nlen))
              (then
                (i32.store (i32.const 0)
                  (i32.add (i32.const 4096) (local.get $line_start)))
                (i32.store (i32.const 4)
                  (i32.add (i32.sub (local.get $i) (local.get $line_start))
                           (i32.const 1)))
                (drop (call $fd_write (i32.const 1) (i32.const 0)
                                      (i32.const 1) (i32.const 12)))))
            (local.set $line_start (i32.add (local.get $i) (i32.const 1)))))
        (local.set $i (i32.add (local.get $i) (i32.const 1)))
        (br $C)))
      (br $L)))
    (call $proc_exit (i32.const 0))
  )
)
