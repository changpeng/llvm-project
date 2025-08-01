; NOTE: Assertions have been autogenerated by utils/update_test_checks.py UTC_ARGS: --version 2
; RUN: opt -passes=loop-vectorize -force-vector-width=1 -force-vector-interleave=2 -S %s | FileCheck %s

define void @test_variable_stride(ptr %dst, i32 %scale) {
; CHECK-LABEL: define void @test_variable_stride
; CHECK-SAME: (ptr [[DST:%.*]], i32 [[SCALE:%.*]]) {
; CHECK-NEXT:  entry:
; CHECK-NEXT:    br i1 false, label [[SCALAR_PH:%.*]], label [[VECTOR_PH:%.*]]
; CHECK:       vector.ph:
; CHECK-NEXT:    br label [[VECTOR_BODY:%.*]]
; CHECK:       vector.body:
; CHECK-NEXT:    [[INDEX:%.*]] = phi i32 [ 0, [[VECTOR_PH]] ], [ [[INDEX_NEXT:%.*]], [[VECTOR_BODY]] ]
; CHECK-NEXT:    [[TMP0:%.*]] = add i32 [[INDEX]], 1
; CHECK-NEXT:    [[TMP1:%.*]] = mul i32 [[INDEX]], [[SCALE]]
; CHECK-NEXT:    [[TMP2:%.*]] = mul i32 [[TMP0]], [[SCALE]]
; CHECK-NEXT:    [[TMP3:%.*]] = getelementptr i16, ptr [[DST]], i32 [[TMP1]]
; CHECK-NEXT:    [[TMP4:%.*]] = getelementptr i16, ptr [[DST]], i32 [[TMP2]]
; CHECK-NEXT:    store i32 [[INDEX]], ptr [[TMP3]], align 2
; CHECK-NEXT:    store i32 [[TMP0]], ptr [[TMP4]], align 2
; CHECK-NEXT:    [[INDEX_NEXT]] = add nuw i32 [[INDEX]], 2
; CHECK-NEXT:    [[TMP5:%.*]] = icmp eq i32 [[INDEX_NEXT]], 1000
; CHECK-NEXT:    br i1 [[TMP5]], label [[MIDDLE_BLOCK:%.*]], label [[VECTOR_BODY]], !llvm.loop [[LOOP0:![0-9]+]]
; CHECK:       middle.block:
; CHECK-NEXT:    br label [[EXIT:%.*]]
; CHECK:       scalar.ph:
; CHECK-NEXT:    [[BC_RESUME_VAL:%.*]] = phi i32 [ 0, [[ENTRY:%.*]] ]
; CHECK-NEXT:    br label [[LOOP:%.*]]
; CHECK:       loop:
; CHECK-NEXT:    [[IV:%.*]] = phi i32 [ [[BC_RESUME_VAL]], [[SCALAR_PH]] ], [ [[IV_NEXT:%.*]], [[LOOP]] ]
; CHECK-NEXT:    [[IDX:%.*]] = mul i32 [[IV]], [[SCALE]]
; CHECK-NEXT:    [[GEP:%.*]] = getelementptr i16, ptr [[DST]], i32 [[IDX]]
; CHECK-NEXT:    store i32 [[IV]], ptr [[GEP]], align 2
; CHECK-NEXT:    [[IV_NEXT]] = add i32 [[IV]], 1
; CHECK-NEXT:    [[EC:%.*]] = icmp eq i32 [[IV_NEXT]], 1000
; CHECK-NEXT:    br i1 [[EC]], label [[EXIT]], label [[LOOP]], !llvm.loop [[LOOP3:![0-9]+]]
; CHECK:       exit:
; CHECK-NEXT:    ret void
;
entry:
  br label %loop

loop:
  %iv = phi i32 [ 0, %entry ], [ %iv.next, %loop ]
  %idx = mul i32 %iv, %scale
  %gep = getelementptr i16, ptr %dst, i32 %idx
  store i32 %iv, ptr %gep, align 2
  %iv.next = add i32 %iv, 1
  %ec = icmp eq i32 %iv.next, 1000
  br i1 %ec, label %exit, label %loop

exit:
  ret void
}
