;; fib.wat — prints fib(30) = 832040 as ASCII to stdout via WASI.
(module
  (import "wasi_snapshot_preview1" "fd_write"
    (func $fd_write (param i32 i32 i32 i32) (result i32)))
  (import "wasi_snapshot_preview1" "proc_exit"
    (func $proc_exit (param i32)))
  (memory (export "memory") 1)

  ;; itoa: writes decimal of $n at $buf, returns length.
  (func $itoa (param $n i32) (param $buf i32) (result i32)
    (local $len i32) (local $i i32) (local $j i32) (local $tmp i32)
    (if (i32.eqz (local.get $n))
      (then
        (i32.store8 (local.get $buf) (i32.const 48))
        (return (i32.const 1))))
    ;; write digits in reverse
    (local.set $len (i32.const 0))
    (block $done (loop $L
      (br_if $done (i32.eqz (local.get $n)))
      (i32.store8
        (i32.add (local.get $buf) (local.get $len))
        (i32.add (i32.const 48) (i32.rem_u (local.get $n) (i32.const 10))))
      (local.set $n (i32.div_u (local.get $n) (i32.const 10)))
      (local.set $len (i32.add (local.get $len) (i32.const 1)))
      (br $L)))
    ;; reverse in place
    (local.set $i (i32.const 0))
    (local.set $j (i32.sub (local.get $len) (i32.const 1)))
    (block $rdone (loop $R
      (br_if $rdone (i32.ge_s (local.get $i) (local.get $j)))
      (local.set $tmp
        (i32.load8_u (i32.add (local.get $buf) (local.get $i))))
      (i32.store8 (i32.add (local.get $buf) (local.get $i))
        (i32.load8_u (i32.add (local.get $buf) (local.get $j))))
      (i32.store8 (i32.add (local.get $buf) (local.get $j))
        (local.get $tmp))
      (local.set $i (i32.add (local.get $i) (i32.const 1)))
      (local.set $j (i32.sub (local.get $j) (i32.const 1)))
      (br $R)))
    (local.get $len))

  (func $fib (param $n i32) (result i32)
    (local $a i32) (local $b i32) (local $t i32)
    (local.set $a (i32.const 0))
    (local.set $b (i32.const 1))
    (block $done (loop $L
      (br_if $done (i32.eqz (local.get $n)))
      (local.set $t (i32.add (local.get $a) (local.get $b)))
      (local.set $a (local.get $b))
      (local.set $b (local.get $t))
      (local.set $n (i32.sub (local.get $n) (i32.const 1)))
      (br $L)))
    (local.get $a))

  (func $_start (export "_start")
    (local $len i32)
    ;; write "fib(30) = " at offset 32, then number, then '\n'.
    (i32.store8 (i32.const 32) (i32.const 102))  ;; f
    (i32.store8 (i32.const 33) (i32.const 105))  ;; i
    (i32.store8 (i32.const 34) (i32.const 98))   ;; b
    (i32.store8 (i32.const 35) (i32.const 40))   ;; (
    (i32.store8 (i32.const 36) (i32.const 51))   ;; 3
    (i32.store8 (i32.const 37) (i32.const 48))   ;; 0
    (i32.store8 (i32.const 38) (i32.const 41))   ;; )
    (i32.store8 (i32.const 39) (i32.const 32))   ;; space
    (i32.store8 (i32.const 40) (i32.const 61))   ;; =
    (i32.store8 (i32.const 41) (i32.const 32))   ;; space
    (local.set $len
      (call $itoa (call $fib (i32.const 30)) (i32.const 42)))
    (i32.store8
      (i32.add (i32.const 42) (local.get $len))
      (i32.const 10))  ;; \n
    (local.set $len (i32.add (local.get $len) (i32.const 11)))
    ;; iov: buf=32, len
    (i32.store (i32.const 0) (i32.const 32))
    (i32.store (i32.const 4) (local.get $len))
    (drop (call $fd_write (i32.const 1) (i32.const 0) (i32.const 1) (i32.const 8)))
    (call $proc_exit (i32.const 0))
  )
)
