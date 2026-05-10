;; rev.wat — reverse each line from stdin (POSIX rev). Reads up to 4 KiB
;; lines, finds '\n', reverses bytes in [start..nl), emits "<reversed>\n".
(module
  (import "wasi_snapshot_preview1" "fd_read"
    (func $fd_read (param i32 i32 i32 i32) (result i32)))
  (import "wasi_snapshot_preview1" "fd_write"
    (func $fd_write (param i32 i32 i32 i32) (result i32)))
  (import "wasi_snapshot_preview1" "proc_exit"
    (func $proc_exit (param i32)))
  (memory (export "memory") 1)

  (func $_start (export "_start")
    (local $n i32) (local $i i32) (local $line_start i32)
    (local $j i32) (local $tmp i32)
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
        (if (i32.eq (i32.load8_u (i32.add (i32.const 4096) (local.get $i)))
                    (i32.const 10))
          (then
            ;; reverse bytes in buffer[line_start..i) in place
            (local.set $j (i32.sub (local.get $i) (i32.const 1)))
            (block $rd (loop $R
              (br_if $rd (i32.ge_s (local.get $line_start) (local.get $j)))
              (local.set $tmp
                (i32.load8_u (i32.add (i32.const 4096) (local.get $line_start))))
              (i32.store8 (i32.add (i32.const 4096) (local.get $line_start))
                (i32.load8_u (i32.add (i32.const 4096) (local.get $j))))
              (i32.store8 (i32.add (i32.const 4096) (local.get $j)) (local.get $tmp))
              (local.set $line_start (i32.add (local.get $line_start) (i32.const 1)))
              (local.set $j (i32.sub (local.get $j) (i32.const 1)))
              (br $R)))
            (local.set $line_start (i32.add (local.get $i) (i32.const 1)))))
        (local.set $i (i32.add (local.get $i) (i32.const 1)))
        (br $C)))
      (i32.store (i32.const 0) (i32.const 4096))
      (i32.store (i32.const 4) (local.get $n))
      (drop (call $fd_write (i32.const 1) (i32.const 0) (i32.const 1) (i32.const 12)))
      (br $L)))
    (call $proc_exit (i32.const 0))
  )
)
