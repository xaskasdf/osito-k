;; tail.wat — print last N lines from stdin (N from argv[1] or -n N,
;; default 10). Implementation: buffer entire input then walk backward.
;; Limited to 64 KiB input for simplicity.
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
  (memory (export "memory") 2)  ;; 128 KiB so 64 KiB input + scratch fits

  (func $atoi (param $p i32) (result i32)
    (local $n i32) (local $c i32)
    (block $d (loop $L
      (local.set $c (i32.load8_u (local.get $p)))
      (br_if $d (i32.eqz (local.get $c)))
      (br_if $d (i32.lt_u (local.get $c) (i32.const 48)))
      (br_if $d (i32.gt_u (local.get $c) (i32.const 57)))
      (local.set $n (i32.add (i32.mul (local.get $n) (i32.const 10))
                              (i32.sub (local.get $c) (i32.const 48))))
      (local.set $p (i32.add (local.get $p) (i32.const 1)))
      (br $L)))
    (local.get $n))

  (func $_start (export "_start")
    (local $argc i32) (local $N i32) (local $argp i32)
    (local $total i32) (local $n i32)
    (local $i i32) (local $nl_count i32) (local $start i32)
    ;; Parse argv
    (drop (call $args_sizes_get (i32.const 16) (i32.const 20)))
    (local.set $argc (i32.load (i32.const 16)))
    (drop (call $args_get (i32.const 64) (i32.const 128)))
    (local.set $N (i32.const 10))
    (if (i32.ge_s (local.get $argc) (i32.const 2))
      (then
        (local.set $argp (i32.load (i32.add (i32.const 64) (i32.const 4))))
        (if (i32.eq (i32.load8_u (local.get $argp)) (i32.const 45))
          (then (if (i32.ge_s (local.get $argc) (i32.const 3))
                  (then (local.set $argp
                          (i32.load (i32.add (i32.const 64) (i32.const 8))))))))
        (local.set $N (call $atoi (local.get $argp)))
        (if (i32.eqz (local.get $N)) (then (local.set $N (i32.const 10))))))
    ;; Read entire stdin into buffer at offset 4096, max 64 KiB
    (local.set $total (i32.const 0))
    (block $eof (loop $L
      (i32.store (i32.const 0)
        (i32.add (i32.const 4096) (local.get $total)))
      (i32.store (i32.const 4)
        (i32.sub (i32.const 65536) (local.get $total)))
      (br_if $eof (i32.ge_s (local.get $total) (i32.const 65536)))
      (drop (call $fd_read (i32.const 0) (i32.const 0)
                            (i32.const 1) (i32.const 8)))
      (local.set $n (i32.load (i32.const 8)))
      (br_if $eof (i32.eqz (local.get $n)))
      (local.set $total (i32.add (local.get $total) (local.get $n)))
      (br $L)))
    ;; Walk backward counting newlines; find start of (last N)th line
    (local.set $i (i32.sub (local.get $total) (i32.const 1)))
    (local.set $nl_count (i32.const 0))
    (local.set $start (i32.const 0))
    (block $found (loop $W
      (br_if $found (i32.lt_s (local.get $i) (i32.const 0)))
      (if (i32.eq (i32.load8_u (i32.add (i32.const 4096) (local.get $i)))
                  (i32.const 10))
        (then
          (local.set $nl_count (i32.add (local.get $nl_count) (i32.const 1)))
          (if (i32.gt_s (local.get $nl_count) (local.get $N))
            (then (local.set $start (i32.add (local.get $i) (i32.const 1)))
                  (br $found)))))
      (local.set $i (i32.sub (local.get $i) (i32.const 1)))
      (br $W)))
    ;; Emit buffer[start..total]
    (i32.store (i32.const 0) (i32.add (i32.const 4096) (local.get $start)))
    (i32.store (i32.const 4) (i32.sub (local.get $total) (local.get $start)))
    (drop (call $fd_write (i32.const 1) (i32.const 0)
                          (i32.const 1) (i32.const 12)))
    (call $proc_exit (i32.const 0))
  )
)
