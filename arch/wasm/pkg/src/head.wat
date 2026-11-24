;; head.wat — print first N lines from stdin. N defaults to 10; if
;; argv[1] starts with '-' skip ('-n' style), parse argv[2] as int.
;; If argv[1] is a positive integer, use it directly (head 5).
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
    (local $argc i32) (local $limit i32) (local $emitted i32)
    (local $n i32) (local $i i32) (local $line_start i32)
    (local $argp i32)
    (local.set $limit (i32.const 10))
    (drop (call $args_sizes_get (i32.const 16) (i32.const 20)))
    (local.set $argc (i32.load (i32.const 16)))
    (drop (call $args_get (i32.const 64) (i32.const 128)))
    (if (i32.ge_s (local.get $argc) (i32.const 2))
      (then
        (local.set $argp (i32.load (i32.add (i32.const 64) (i32.const 4))))
        ;; If first byte is '-', look at argv[2]; else parse as N.
        (if (i32.eq (i32.load8_u (local.get $argp)) (i32.const 45))
          (then
            (if (i32.ge_s (local.get $argc) (i32.const 3))
              (then (local.set $argp
                      (i32.load (i32.add (i32.const 64) (i32.const 8)))))))
          (else))
        (local.set $limit (call $atoi (local.get $argp)))
        (if (i32.eqz (local.get $limit))
          (then (local.set $limit (i32.const 10))))))
    (local.set $emitted (i32.const 0))
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
        (br_if $cd (i32.ge_s (local.get $emitted) (local.get $limit)))
        (if (i32.eq (i32.load8_u (i32.add (i32.const 4096) (local.get $i)))
                    (i32.const 10))
          (then
            (i32.store (i32.const 0)
              (i32.add (i32.const 4096) (local.get $line_start)))
            (i32.store (i32.const 4)
              (i32.add (i32.sub (local.get $i) (local.get $line_start))
                       (i32.const 1)))
            (drop (call $fd_write (i32.const 1) (i32.const 0)
                                  (i32.const 1) (i32.const 12)))
            (local.set $emitted (i32.add (local.get $emitted) (i32.const 1)))
            (local.set $line_start (i32.add (local.get $i) (i32.const 1)))))
        (local.set $i (i32.add (local.get $i) (i32.const 1)))
        (br $C)))
      (br_if $eof (i32.ge_s (local.get $emitted) (local.get $limit)))
      (br $L)))
    (call $proc_exit (i32.const 0))
  )
)
