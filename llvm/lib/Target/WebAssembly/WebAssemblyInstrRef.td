// WebAssemblyInstrRef.td - WebAssembly reference type codegen --*- tablegen -*-
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
///
/// \file
/// WebAssembly reference type operand codegen constructs.
///
//===----------------------------------------------------------------------===//

multiclass REF_I<string name, string ht> {
  defm REF_NULL_#name : I<(outs WASMREF:$dst), (ins),
                          (outs), (ins),
                          [(set WASMREF:$dst, (!cast<Intrinsic>("int_wasm_ref_null_" # ht)))],
                          "ref.null_" # ht # "$dst",
                          "ref.null_" # ht,
                          !cond(!eq(ht, "func")   : 0xd070,
                                !eq(ht, "extern") : 0xd06f)>,
                      Requires<[HasReferenceTypes]>;
}

defm "" : REF_I<"FUNCREF", "func">;
defm "" : REF_I<"EXTERNREF", "extern">;

defm REF_IS_NULL
    : I<(outs I32:$dst), (ins WASMREF:$ref), (outs), (ins),
        [(set I32:$dst, (int_wasm_ref_is_null WASMREF:$ref))],
        "ref.is_null\t$ref",
        "ref.is_null", 0xd1>,
      Requires<[HasReferenceTypes]>;
