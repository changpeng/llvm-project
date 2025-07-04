; NOTE: Assertions have been autogenerated by utils/update_llc_test_checks.py
; RUN: llc -mtriple=armv6-eabi %s -o - | FileCheck %s --check-prefixes=CHECK,ARMV6
; RUN: llc -mtriple=thumbv8.1m.main-arm-none-eabi -mattr=+dsp %s -o - | FileCheck %s --check-prefixes=CHECK,THUMB

define arm_aapcs_vfpcc i32 @usat_lsl(i32 %num){
; CHECK-LABEL: usat_lsl:
; CHECK:       @ %bb.0: @ %entry
; CHECK-NEXT:    usat r0, #7, r0, lsl #2
; CHECK-NEXT:    bx lr
entry:
  %shl = shl i32 %num, 2
  %0 = tail call i32 @llvm.arm.usat(i32 %shl, i32 7)
  ret i32 %0
}

define arm_aapcs_vfpcc i32 @usat_asr(i32 %num){
; CHECK-LABEL: usat_asr:
; CHECK:       @ %bb.0: @ %entry
; CHECK-NEXT:    usat r0, #7, r0, asr #2
; CHECK-NEXT:    bx lr
entry:
  %shr = ashr i32 %num, 2
  %0 = tail call i32 @llvm.arm.usat(i32 %shr, i32 7)
  ret i32 %0
}

define arm_aapcs_vfpcc i32 @usat_lsl2(i32 %num){
; ARMV6-LABEL: usat_lsl2:
; ARMV6:       @ %bb.0: @ %entry
; ARMV6-NEXT:    lsl r0, r0, #15
; ARMV6-NEXT:    bic r1, r0, r0, asr #31
; ARMV6-NEXT:    mov r0, #255
; ARMV6-NEXT:    orr r0, r0, #32512
; ARMV6-NEXT:    cmp r1, r0
; ARMV6-NEXT:    movlt r0, r1
; ARMV6-NEXT:    bx lr
;
; THUMB-LABEL: usat_lsl2:
; THUMB:       @ %bb.0: @ %entry
; THUMB-NEXT:    lsls r0, r0, #15
; THUMB-NEXT:    movw r1, #32767
; THUMB-NEXT:    bic.w r0, r0, r0, asr #31
; THUMB-NEXT:    cmp r0, r1
; THUMB-NEXT:    csel r0, r0, r1, lt
; THUMB-NEXT:    bx lr
entry:
  %shl = shl nsw i32 %num, 15
  %0 = icmp sgt i32 %shl, 0
  %1 = select i1 %0, i32 %shl, i32 0
  %2 = icmp slt i32 %1, 32767
  %3 = select i1 %2, i32 %1, i32 32767
  ret i32 %3
}

define arm_aapcs_vfpcc i32 @usat_asr2(i32 %num){
; ARMV6-LABEL: usat_asr2:
; ARMV6:       @ %bb.0: @ %entry
; ARMV6-NEXT:    asr r1, r0, #15
; ARMV6-NEXT:    bic r1, r1, r0, asr #31
; ARMV6-NEXT:    mov r0, #255
; ARMV6-NEXT:    orr r0, r0, #32512
; ARMV6-NEXT:    cmp r1, r0
; ARMV6-NEXT:    movlt r0, r1
; ARMV6-NEXT:    bx lr
;
; THUMB-LABEL: usat_asr2:
; THUMB:       @ %bb.0: @ %entry
; THUMB-NEXT:    asrs r1, r0, #15
; THUMB-NEXT:    bic.w r0, r1, r0, asr #31
; THUMB-NEXT:    movw r1, #32767
; THUMB-NEXT:    cmp r0, r1
; THUMB-NEXT:    csel r0, r0, r1, lt
; THUMB-NEXT:    bx lr
entry:
  %shr = ashr i32 %num, 15
  %0 = icmp sgt i32 %shr, 0
  %1 = select i1 %0, i32 %shr, i32 0
  %2 = icmp slt i32 %1, 32767
  %3 = select i1 %2, i32 %1, i32 32767
  ret i32 %3
}

declare i32 @llvm.arm.usat(i32, i32)
