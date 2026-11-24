;; base64.wat — encode stdin to base64. No decode (single-direction).
;; Output formatted as 76-column lines (RFC 2045 style).
(module
  (import "wasi_snapshot_preview1" "fd_read"
    (func $fd_read (param i32 i32 i32 i32) (result i32)))
  (import "wasi_snapshot_preview1" "fd_write"
    (func $fd_write (param i32 i32 i32 i32) (result i32)))
  (import "wasi_snapshot_preview1" "proc_exit"
    (func $proc_exit (param i32)))
  (memory (export "memory") 1)

  ;; Base64 alphabet at offset 256:
  ;; ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/
  (data (i32.const 256)
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/")

  (func $emit (param $c i32)
    (i32.store8 (i32.const 16) (local.get $c))
    (i32.store (i32.const 0) (i32.const 16))
    (i32.store (i32.const 4) (i32.const 1))
    (drop (call $fd_write (i32.const 1) (i32.const 0)
                          (i32.const 1) (i32.const 12))))

  (func $emit_b64 (param $v i32)
    (call $emit
      (i32.load8_u (i32.add (i32.const 256) (local.get $v)))))

  (func $_start (export "_start")
    (local $n i32) (local $i i32) (local $col i32)
    (local $b1 i32) (local $b2 i32) (local $b3 i32)
    (local $rem i32)
    (local.set $col (i32.const 0))
    (block $eof (loop $L
      (i32.store (i32.const 0) (i32.const 4096))
      (i32.store (i32.const 4) (i32.const 4095))  ;; multiple of 3
      (drop (call $fd_read (i32.const 0) (i32.const 0)
                            (i32.const 1) (i32.const 8)))
      (local.set $n (i32.load (i32.const 8)))
      (br_if $eof (i32.eqz (local.get $n)))
      ;; Encode triples
      (local.set $i (i32.const 0))
      (block $cd (loop $C
        (br_if $cd (i32.gt_s (i32.add (local.get $i) (i32.const 3))
                              (local.get $n)))
        (local.set $b1 (i32.load8_u (i32.add (i32.const 4096) (local.get $i))))
        (local.set $b2 (i32.load8_u (i32.add (i32.const 4096)
                                              (i32.add (local.get $i) (i32.const 1)))))
        (local.set $b3 (i32.load8_u (i32.add (i32.const 4096)
                                              (i32.add (local.get $i) (i32.const 2)))))
        (call $emit_b64 (i32.shr_u (local.get $b1) (i32.const 2)))
        (call $emit_b64 (i32.and (i32.or
                          (i32.shl (i32.and (local.get $b1) (i32.const 3)) (i32.const 4))
                          (i32.shr_u (local.get $b2) (i32.const 4)))
                          (i32.const 63)))
        (call $emit_b64 (i32.and (i32.or
                          (i32.shl (i32.and (local.get $b2) (i32.const 15)) (i32.const 2))
                          (i32.shr_u (local.get $b3) (i32.const 6)))
                          (i32.const 63)))
        (call $emit_b64 (i32.and (local.get $b3) (i32.const 63)))
        (local.set $col (i32.add (local.get $col) (i32.const 4)))
        (if (i32.ge_s (local.get $col) (i32.const 76))
          (then (call $emit (i32.const 10))
                (local.set $col (i32.const 0))))
        (local.set $i (i32.add (local.get $i) (i32.const 3)))
        (br $C)))
      ;; Handle trailing 1 or 2 bytes (final read only)
      (local.set $rem (i32.sub (local.get $n) (local.get $i)))
      (if (i32.eq (local.get $rem) (i32.const 1))
        (then
          (local.set $b1 (i32.load8_u (i32.add (i32.const 4096) (local.get $i))))
          (call $emit_b64 (i32.shr_u (local.get $b1) (i32.const 2)))
          (call $emit_b64 (i32.and
                            (i32.shl (i32.and (local.get $b1) (i32.const 3))
                                     (i32.const 4))
                            (i32.const 63)))
          (call $emit (i32.const 61))
          (call $emit (i32.const 61))))
      (if (i32.eq (local.get $rem) (i32.const 2))
        (then
          (local.set $b1 (i32.load8_u (i32.add (i32.const 4096) (local.get $i))))
          (local.set $b2 (i32.load8_u (i32.add (i32.const 4096)
                                                (i32.add (local.get $i) (i32.const 1)))))
          (call $emit_b64 (i32.shr_u (local.get $b1) (i32.const 2)))
          (call $emit_b64 (i32.and (i32.or
                            (i32.shl (i32.and (local.get $b1) (i32.const 3)) (i32.const 4))
                            (i32.shr_u (local.get $b2) (i32.const 4)))
                            (i32.const 63)))
          (call $emit_b64 (i32.and
                            (i32.shl (i32.and (local.get $b2) (i32.const 15))
                                     (i32.const 2))
                            (i32.const 63)))
          (call $emit (i32.const 61))))
      (br $L)))
    (call $emit (i32.const 10))   ;; trailing newline
    (call $proc_exit (i32.const 0))
  )
)
