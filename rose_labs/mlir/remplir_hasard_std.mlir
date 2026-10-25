// MLIR standard-dialect representation of remplir_hasard (1 draw = 8 bytes).
// RNG splitmix64 correct. Utilise uniquement les dialectes built-in.

module {
  func.func @remplir_hasard(%buf: memref<?xi64>, %n: index, %state0: i64) -> i64 {
    %c0   = arith.constant 0 : index
    %c1   = arith.constant 1 : index
    %mask = arith.constant 0x9E3779B97F4A7C15 : i64
    %m1   = arith.constant 0xBF58476D1CE4E5B9 : i64
    %m2   = arith.constant 0x94D049BB133111B9 : i64
    %sh30 = arith.constant 30 : i64
    %sh27 = arith.constant 27 : i64
    %sh31 = arith.constant 31 : i64

    %final = scf.for %i = %c0 to %n step %c1 iter_args(%st = %state0) -> (i64) {
      %s1 = arith.addi %st, %mask : i64
      %t1 = arith.shrui %s1, %sh30 : i64
      %z2 = arith.xori %s1, %t1 : i64
      %z3 = arith.muli %z2, %m1 : i64
      %t2 = arith.shrui %z3, %sh27 : i64
      %z4 = arith.xori %z3, %t2 : i64
      %z5 = arith.muli %z4, %m2 : i64
      %t3 = arith.shrui %z5, %sh31 : i64
      %out = arith.xori %z5, %t3 : i64
      memref.store %out, %buf[%i] : memref<?xi64>
      scf.yield %s1 : i64
    }
    return %final : i64
  }
}