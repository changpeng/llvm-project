; NOTE: Assertions have been autogenerated by utils/update_llc_test_checks.py
; RUN: llc < %s -mtriple=powerpc64le-unknown-linux-gnu -verify-machineinstrs\
; RUN:       -mcpu=pwr9 --ppc-enable-pipeliner | FileCheck %s

define void @lame_encode_buffer_interleaved(ptr %arg0) local_unnamed_addr {
; CHECK-LABEL: lame_encode_buffer_interleaved:
; CHECK:       # %bb.0:
; CHECK-NEXT:    li 4, 1
; CHECK-NEXT:    rldic 4, 4, 62, 1
; CHECK-NEXT:    mtctr 4
; CHECK-NEXT:    .p2align 4
; CHECK-NEXT:  .LBB0_1:
; CHECK-NEXT:    lha 4, 0(3)
; CHECK-NEXT:    lha 5, 0(3)
; CHECK-NEXT:    srawi 4, 4, 1
; CHECK-NEXT:    addze 4, 4
; CHECK-NEXT:    srawi 5, 5, 1
; CHECK-NEXT:    addze 5, 5
; CHECK-NEXT:    sth 4, 0(3)
; CHECK-NEXT:    sth 5, 0(3)
; CHECK-NEXT:    bdnz .LBB0_1
; CHECK-NEXT:  # %bb.2:
; CHECK-NEXT:    blr
  %undef = freeze ptr poison
  br label %1

1:                                                ; preds = %1, %0
  %2 = phi i64 [ 0, %0 ], [ %13, %1 ]
  %3 = load i16, ptr %arg0, align 2
  %4 = load i16, ptr %undef, align 2
  %5 = sext i16 %3 to i32
  %6 = sext i16 %4 to i32
  %7 = add nsw i32 0, %5
  %8 = add nsw i32 0, %6
  %9 = sdiv i32 %7, 2
  %10 = sdiv i32 %8, 2
  %11 = trunc i32 %9 to i16
  %12 = trunc i32 %10 to i16
  store i16 %11, ptr %arg0, align 2
  store i16 %12, ptr %undef, align 2
  %13 = add i64 %2, 4
  %14 = icmp eq i64 %13, 0
  br i1 %14, label %15, label %1

15:                                               ; preds = %1
  ret void
}
