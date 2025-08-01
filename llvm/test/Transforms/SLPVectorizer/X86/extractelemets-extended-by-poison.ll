; NOTE: Assertions have been autogenerated by utils/update_test_checks.py UTC_ARGS: --version 5
; RUN: opt -S --passes=slp-vectorizer -mtriple=x86_64-unknown-linux-gnu < %s | FileCheck %s

define i32 @test() {
; CHECK-LABEL: define i32 @test() {
; CHECK-NEXT:  [[ENTRY:.*:]]
; CHECK-NEXT:    [[TMP0:%.*]] = load <4 x i64>, ptr null, align 16
; CHECK-NEXT:    [[TMP12:%.*]] = extractelement <4 x i64> [[TMP0]], i32 1
; CHECK-NEXT:    [[TMP13:%.*]] = or i64 [[TMP12]], 0
; CHECK-NEXT:    [[TMP2:%.*]] = shufflevector <4 x i64> [[TMP0]], <4 x i64> poison, <8 x i32> <i32 poison, i32 poison, i32 poison, i32 poison, i32 poison, i32 1, i32 poison, i32 poison>
; CHECK-NEXT:    [[TMP3:%.*]] = shufflevector <4 x i64> [[TMP0]], <4 x i64> poison, <8 x i32> <i32 0, i32 1, i32 2, i32 3, i32 poison, i32 poison, i32 poison, i32 poison>
; CHECK-NEXT:    [[TMP4:%.*]] = shufflevector <8 x i64> [[TMP3]], <8 x i64> <i64 poison, i64 poison, i64 poison, i64 poison, i64 0, i64 poison, i64 poison, i64 poison>, <8 x i32> <i32 poison, i32 poison, i32 poison, i32 poison, i32 12, i32 1, i32 poison, i32 poison>
; CHECK-NEXT:    [[TMP5:%.*]] = shufflevector <8 x i64> [[TMP4]], <8 x i64> [[TMP3]], <8 x i32> <i32 8, i32 9, i32 10, i32 11, i32 4, i32 5, i32 poison, i32 poison>
; CHECK-NEXT:    [[TMP6:%.*]] = trunc <8 x i64> [[TMP5]] to <8 x i32>
; CHECK-NEXT:    [[TMP7:%.*]] = shufflevector <8 x i32> [[TMP6]], <8 x i32> poison, <16 x i32> <i32 0, i32 0, i32 0, i32 1, i32 1, i32 1, i32 2, i32 2, i32 2, i32 3, i32 3, i32 3, i32 4, i32 4, i32 5, i32 5>
; CHECK-NEXT:    [[TMP1:%.*]] = shufflevector <4 x i64> [[TMP0]], <4 x i64> poison, <8 x i32> <i32 1, i32 2, i32 2, i32 3, i32 3, i32 3, i32 2, i32 1>
; CHECK-NEXT:    [[TMP14:%.*]] = trunc <8 x i64> [[TMP1]] to <8 x i32>
; CHECK-NEXT:    [[TMP15:%.*]] = add <8 x i32> [[TMP14]], zeroinitializer
; CHECK-NEXT:    [[TMP8:%.*]] = add <16 x i32> [[TMP7]], zeroinitializer
; CHECK-NEXT:    [[TMP9:%.*]] = extractelement <4 x i64> [[TMP0]], i32 0
; CHECK-NEXT:    [[INC_3_3_I_1:%.*]] = or i64 [[TMP9]], 0
; CHECK-NEXT:    [[TMP16:%.*]] = shufflevector <16 x i32> [[TMP8]], <16 x i32> poison, <8 x i32> <i32 0, i32 1, i32 2, i32 3, i32 4, i32 5, i32 6, i32 7>
; CHECK-NEXT:    [[RDX_OP:%.*]] = or <8 x i32> [[TMP16]], [[TMP15]]
; CHECK-NEXT:    [[TMP18:%.*]] = shufflevector <8 x i32> [[RDX_OP]], <8 x i32> poison, <16 x i32> <i32 0, i32 1, i32 2, i32 3, i32 4, i32 5, i32 6, i32 7, i32 poison, i32 poison, i32 poison, i32 poison, i32 poison, i32 poison, i32 poison, i32 poison>
; CHECK-NEXT:    [[TMP17:%.*]] = shufflevector <16 x i32> [[TMP8]], <16 x i32> [[TMP18]], <16 x i32> <i32 16, i32 17, i32 18, i32 19, i32 20, i32 21, i32 22, i32 23, i32 8, i32 9, i32 10, i32 11, i32 12, i32 13, i32 14, i32 15>
; CHECK-NEXT:    [[OP_RDX:%.*]] = call i32 @llvm.vector.reduce.or.v16i32(<16 x i32> [[TMP17]])
; CHECK-NEXT:    ret i32 [[OP_RDX]]
;
entry:
  %.pre.i = load i64, ptr getelementptr inbounds nuw (i8, ptr null, i64 24), align 8
  %.pre50.i = load i64, ptr getelementptr inbounds nuw (i8, ptr null, i64 16), align 16
  %.pre51.i = load i64, ptr getelementptr inbounds nuw (i8, ptr null, i64 8), align 8
  %.pre52.i = load i64, ptr null, align 16
  %0 = or i64 %.pre51.i, 0
  %1 = trunc i64 %.pre.i to i32
  %2 = add i32 %1, 0
  %3 = trunc i64 %.pre50.i to i32
  %4 = add i32 %3, 0
  %5 = trunc i64 %.pre51.i to i32
  %6 = add i32 %5, 0
  %7 = trunc i64 0 to i32
  %8 = add i32 %5, 0
  %9 = add i32 %7, 0
  %10 = add i32 %1, 0
  %11 = add i32 %3, 0
  %12 = add i32 %5, 0
  %13 = add i32 %7, 0
  %14 = trunc i64 %.pre.i to i32
  %15 = add i32 %14, 0
  %16 = trunc i64 %.pre50.i to i32
  %17 = add i32 %16, 0
  %18 = trunc i64 %.pre51.i to i32
  %19 = add i32 %18, 0
  %20 = trunc i64 %.pre52.i to i32
  %conv14.1.i = or i32 %9, %13
  %21 = or i32 %conv14.1.i, %6
  %22 = or i32 %21, %8
  %23 = or i32 %22, %12
  %24 = or i32 %23, %4
  %25 = or i32 %24, %11
  %26 = or i32 %25, %2
  %27 = or i32 %26, %10
  %28 = or i32 %27, %15
  %29 = or i32 %28, %17
  %30 = or i32 %29, %19
  %31 = add i32 %14, 0
  %32 = add i32 %16, 0
  %33 = add i32 %18, 0
  %34 = add i32 %20, 0
  %35 = add i32 %14, 0
  %36 = add i32 %16, 0
  %37 = add i32 %18, 0
  %38 = add i32 %20, 0
  %39 = add i32 %14, 0
  %40 = add i32 %16, 0
  %41 = add i32 %18, 0
  %42 = add i32 %20, 0
  %inc.3.3.i.1 = or i64 %.pre52.i, 0
  %conv14.i.1 = or i32 %38, %34
  %conv14.1.i.1 = or i32 %conv14.i.1, %42
  %conv14.3.i.1 = or i32 %conv14.1.i.1, %33
  %conv14.145.i.1 = or i32 %conv14.3.i.1, %37
  %conv14.1.1.i.1 = or i32 %conv14.145.i.1, %41
  %conv14.3.1.i.1 = or i32 %conv14.1.1.i.1, %32
  %conv14.247.i.1 = or i32 %conv14.3.1.i.1, %36
  %conv14.1.2.i.1 = or i32 %conv14.247.i.1, %40
  %conv14.3.2.i.1 = or i32 %conv14.1.2.i.1, %31
  %conv14.349.i.1 = or i32 %conv14.3.2.i.1, %35
  %conv14.1.3.i.1 = or i32 %conv14.349.i.1, %39
  %conv14.3.3.i.1 = or i32 %conv14.1.3.i.1, %30
  ret i32 %conv14.3.3.i.1
}
