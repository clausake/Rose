// Squelette MLIR (textuel) — dialecte "rose" hypothétique.
// Non compilable sans libmlir installée. But pédagogique.
//
// Objectif : représenter remplir_hasard au niveau haut pour permettre
// des passes (vectorisation, fusion) avant l'émission LLVM IR.

module {
  func.func @remplir_hasard(%buf: memref<?xi8>, %alpha: memref<?xi8>) {
    %c0 = arith.constant 0 : index
    %n  = memref.dim %buf, %c0 : memref<?xi8>
    %step = arith.constant 32 : index
    scf.for %i = %c0 to %n step %step {
      // 4 tirages splitmix64 + scatter de 8 octets chacun
      %r0 = rose.rng_tirer : i64
      %r1 = rose.rng_tirer : i64
      %r2 = rose.rng_tirer : i64
      %r3 = rose.rng_tirer : i64
      rose.scatter %r0, %r1, %r2, %r3 into %buf[%i] : (i64, i64, i64, i64, memref<?xi8>, index) -> ()
    }
    return
  }
}

// Pipeline visée :
//   rose dialect -> (lowering) scf/memref/arith -> (convert) LLVM dialect
//   -> mlir-translate --mlir-to-llvmir -> .ll -> clang -O3