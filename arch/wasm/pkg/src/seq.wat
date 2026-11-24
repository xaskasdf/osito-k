;; seq.wat — print 1..N (one per line) where N is argv[1]. Useful for
;; testing pipes: `pkg run seq 5 | pkg run head 3` -> 1, 2, 3.
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

  (func $itoa (param $n i32) (param $buf i32) (result i32)
    (local $len i32) (local $i i32) (local $j i32) (local $tmp i32)
    (if (i32.eqz (local.get $n))
      (then (i32.store8 (local.get $buf) (i32.const 48))
            (return (i32.const 1))))
    (block $d (loop $L
      (br_if $d (i32.eqz (local.get $n)))
      (i32.store8 (i32.add (local.get $buf) (local.get $len))
        (i32.add (i32.const 48) (i32.rem_u (local.get $n) (i32.const 10))))
      (local.set $n (i32.div_u (local.get $n) (i32.const 10)))
      (local.set $len (i32.add (local.get $len) (i32.const 1)))
      (br $L)))
    (local.set $i (i32.const 0))
    (local.set $j (i32.sub (local.get $len) (i32.const 1)))
    (block $rd (loop $R
      (br_if $rd (i32.ge_s (local.get $i) (local.get $j)))
      (local.set $tmp (i32.load8_u (i32.add (local.get $buf) (local.get $i))))
      (i32.store8 (i32.add (local.get $buf) (local.get $i))
        (i32.load8_u (i32.add (local.get $buf) (local.get $j))))
      (i32.store8 (i32.add (local.get $buf) (local.get $j)) (local.get $tmp))
      (local.set $i (i32.add (local.get $i) (i32.const 1)))
      (local.set $j (i32.sub (local.get $j) (i32.const 1)))
      (br $R)))
    (local.get $len))

  (func $_start (export "_start")
    (local $argc i32) (local $N i32) (local $i i32) (local $len i32)
    (drop (call $args_sizes_get (i32.const 16) (i32.const 20)))
    (local.set $argc (i32.load (i32.const 16)))
    (drop (call $args_get (i32.const 64) (i32.const 128)))
    (local.set $N (i32.const 10))
    (if (i32.ge_s (local.get $argc) (i32.const 2))
      (then (local.set $N (call $atoi
              (i32.load (i32.add (i32.const 64) (i32.const 4)))))))
    (if (i32.eqz (local.get $N)) (then (call $proc_exit (i32.const 0))))
    (local.set $i (i32.const 1))
    (block $d (loop $L
      (br_if $d (i32.gt_s (local.get $i) (local.get $N)))
      (local.set $len (call $itoa (local.get $i) (i32.const 256)))
      (i32.store8 (i32.add (i32.const 256) (local.get $len)) (i32.const 10))
      (i32.store (i32.const 0) (i32.const 256))
      (i32.store (i32.const 4) (i32.add (local.get $len) (i32.const 1)))
      (drop (call $fd_write (i32.const 1) (i32.const 0) (i32.const 1) (i32.const 12)))
      (local.set $i (i32.add (local.get $i) (i32.const 1)))
      (br $L)))
    (call $proc_exit (i32.const 0))
  )
)
