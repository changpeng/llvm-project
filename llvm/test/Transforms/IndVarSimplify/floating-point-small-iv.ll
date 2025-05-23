; NOTE: Assertions have been autogenerated by utils/update_test_checks.py
; RUN: opt < %s -passes=indvars -S | FileCheck %s

@array = dso_local global [16777219 x i32] zeroinitializer, align 4

define void @sitofp_fptosi_range() {
; CHECK-LABEL: @sitofp_fptosi_range(
; CHECK-NEXT:  entry:
; CHECK-NEXT:    br label [[FOR_BODY:%.*]]
; CHECK:       for.body:
; CHECK-NEXT:    [[IV_INT:%.*]] = phi i32 [ 100, [[ENTRY:%.*]] ], [ [[DEC_INT:%.*]], [[FOR_BODY]] ]
; CHECK-NEXT:    [[IDXPROM:%.*]] = sext i32 [[IV_INT]] to i64
; CHECK-NEXT:    [[ARRAYIDX:%.*]] = getelementptr inbounds [16777219 x i32], ptr @array, i64 0, i64 [[IDXPROM]]
; CHECK-NEXT:    store i32 [[IV_INT]], ptr [[ARRAYIDX]], align 4
; CHECK-NEXT:    [[DEC_INT]] = add nsw i32 [[IV_INT]], -1
; CHECK-NEXT:    [[CMP:%.*]] = icmp ugt i32 [[DEC_INT]], 0
; CHECK-NEXT:    br i1 [[CMP]], label [[FOR_BODY]], label [[CLEANUP:%.*]]
; CHECK:       cleanup:
; CHECK-NEXT:    ret void
;
entry:
  br label %for.body

for.body:                                         ; preds = %for.body, %entry
  %iv.int = phi i32 [ 100, %entry ], [ %dec.int, %for.body ]
  %indvar.conv = sitofp i32 %iv.int to float
  %conv = fptosi float %indvar.conv to i32
  %idxprom = sext i32 %conv to i64
  %arrayidx = getelementptr inbounds [16777219 x i32], ptr @array, i64 0, i64 %idxprom
  store i32 %conv, ptr %arrayidx, align 4
  %dec.int = add nsw i32 %iv.int, -1
  %cmp = icmp ugt i32 %dec.int, 0
  br i1 %cmp, label %for.body, label %cleanup

cleanup:                                          ; preds = %for.body
  ret void
}

; Negative test: The transform is *not* valid because there are too many significant bits
define void @sitofp_fptosi_range_overflow() {
; CHECK-LABEL: @sitofp_fptosi_range_overflow(
; CHECK-NEXT:  entry:
; CHECK-NEXT:    br label [[FOR_BODY:%.*]]
; CHECK:       for.body:
; CHECK-NEXT:    [[IV_INT:%.*]] = phi i32 [ 16777218, [[ENTRY:%.*]] ], [ [[DEC_INT:%.*]], [[FOR_BODY]] ]
; CHECK-NEXT:    [[INDVAR_CONV:%.*]] = sitofp i32 [[IV_INT]] to float
; CHECK-NEXT:    [[CONV:%.*]] = fptosi float [[INDVAR_CONV]] to i32
; CHECK-NEXT:    [[IDXPROM:%.*]] = sext i32 [[CONV]] to i64
; CHECK-NEXT:    [[ARRAYIDX:%.*]] = getelementptr inbounds [16777219 x i32], ptr @array, i64 0, i64 [[IDXPROM]]
; CHECK-NEXT:    store i32 [[CONV]], ptr [[ARRAYIDX]], align 4
; CHECK-NEXT:    [[DEC_INT]] = add nsw i32 [[IV_INT]], -1
; CHECK-NEXT:    [[CMP:%.*]] = icmp ugt i32 [[DEC_INT]], 0
; CHECK-NEXT:    br i1 [[CMP]], label [[FOR_BODY]], label [[CLEANUP:%.*]]
; CHECK:       cleanup:
; CHECK-NEXT:    ret void
;
entry:
  br label %for.body

for.body:                                         ; preds = %for.body, %entry
  %iv.int = phi i32 [ 16777218, %entry ], [ %dec.int, %for.body ] ; intermediate 16777218 (= 1 << 24 + 2)
  %indvar.conv = sitofp i32 %iv.int to float
  %conv = fptosi float %indvar.conv to i32
  %idxprom = sext i32 %conv to i64
  %arrayidx = getelementptr inbounds [16777219 x i32], ptr @array, i64 0, i64 %idxprom
  store i32 %conv, ptr %arrayidx, align 4
  %dec.int = add nsw i32 %iv.int, -1
  %cmp = icmp ugt i32 %dec.int, 0
  br i1 %cmp, label %for.body, label %cleanup

cleanup:                                          ; preds = %for.body
  ret void
}

; Type mismatch between the integer IV and the fptosi result
define void @sitofp_fptosi_range_trunc() {
;
; CHECK-LABEL: @sitofp_fptosi_range_trunc(
; CHECK-NEXT:  entry:
; CHECK-NEXT:    br label [[FOR_BODY:%.*]]
; CHECK:       for.body:
; CHECK-NEXT:    [[IV_INT:%.*]] = phi i64 [ 100, [[ENTRY:%.*]] ], [ [[DEC_INT:%.*]], [[FOR_BODY]] ]
; CHECK-NEXT:    [[IV_INT_TRUNC:%.*]] = trunc i64 [[IV_INT]] to i32
; CHECK-NEXT:    [[ARRAYIDX:%.*]] = getelementptr inbounds [16777219 x i32], ptr @array, i64 0, i64 [[IV_INT]]
; CHECK-NEXT:    store i32 [[IV_INT_TRUNC]], ptr [[ARRAYIDX]], align 4
; CHECK-NEXT:    [[DEC_INT]] = add nsw i64 [[IV_INT]], -1
; CHECK-NEXT:    [[CMP:%.*]] = icmp ugt i64 [[DEC_INT]], 0
; CHECK-NEXT:    br i1 [[CMP]], label [[FOR_BODY]], label [[CLEANUP:%.*]]
; CHECK:       cleanup:
; CHECK-NEXT:    ret void
;
entry:
  br label %for.body

for.body:                                         ; preds = %for.body, %entry
  %iv.int = phi i64 [ 100, %entry ], [ %dec.int, %for.body ]
  %indvar.conv = sitofp i64 %iv.int to float
  %idxprom32 = fptosi float %indvar.conv to i32
  %idxprom64 = fptosi float %indvar.conv to i64
  %arrayidx = getelementptr inbounds [16777219 x i32], ptr @array, i64 0, i64 %idxprom64
  store i32 %idxprom32, ptr %arrayidx, align 4
  %dec.int = add nsw i64 %iv.int, -1
  %cmp = icmp ugt i64 %dec.int, 0
  br i1 %cmp, label %for.body, label %cleanup

cleanup:                                          ; preds = %for.body
  ret void
}

define void @sitofp_fptosi_range_sext() {
;
; CHECK-LABEL: @sitofp_fptosi_range_sext(
; CHECK-NEXT:  entry:
; CHECK-NEXT:    br label [[FOR_BODY:%.*]]
; CHECK:       for.body:
; CHECK-NEXT:    [[IV_INT:%.*]] = phi i16 [ 100, [[ENTRY:%.*]] ], [ [[DEC_INT:%.*]], [[FOR_BODY]] ]
; CHECK-NEXT:    [[IV_INT_SEXT1:%.*]] = sext i16 [[IV_INT]] to i32
; CHECK-NEXT:    [[IV_INT_SEXT:%.*]] = sext i16 [[IV_INT]] to i64
; CHECK-NEXT:    [[ARRAYIDX:%.*]] = getelementptr inbounds [16777219 x i32], ptr @array, i64 0, i64 [[IV_INT_SEXT]]
; CHECK-NEXT:    store i32 [[IV_INT_SEXT1]], ptr [[ARRAYIDX]], align 4
; CHECK-NEXT:    [[DEC_INT]] = add nsw i16 [[IV_INT]], -1
; CHECK-NEXT:    [[CMP:%.*]] = icmp sgt i16 [[DEC_INT]], -3
; CHECK-NEXT:    br i1 [[CMP]], label [[FOR_BODY]], label [[CLEANUP:%.*]]
; CHECK:       cleanup:
; CHECK-NEXT:    ret void
;
entry:
  br label %for.body

for.body:                                         ; preds = %for.body, %entry
  %iv.int = phi i16 [ 100, %entry ], [ %dec.int, %for.body ]
  %indvar.conv = sitofp i16 %iv.int to float
  %idxprom32 = fptosi float %indvar.conv to i32
  %idxprom64 = fptosi float %indvar.conv to i64
  %arrayidx = getelementptr inbounds [16777219 x i32], ptr @array, i64 0, i64 %idxprom64
  store i32 %idxprom32, ptr %arrayidx, align 4
  %dec.int = add nsw i16 %iv.int, -1
  %cmp = icmp sgt i16 %dec.int, -3
  br i1 %cmp, label %for.body, label %cleanup

cleanup:                                          ; preds = %for.body
  ret void
}

; If one of them is unsigned, then we can use zext.
define void @sitofp_fptoui_range_zext() {
;
; CHECK-LABEL: @sitofp_fptoui_range_zext(
; CHECK-NEXT:  entry:
; CHECK-NEXT:    br label [[FOR_BODY:%.*]]
; CHECK:       for.body:
; CHECK-NEXT:    [[IV_INT:%.*]] = phi i16 [ 100, [[ENTRY:%.*]] ], [ [[DEC_INT:%.*]], [[FOR_BODY]] ]
; CHECK-NEXT:    [[IV_INT_ZEXT1:%.*]] = zext i16 [[IV_INT]] to i32
; CHECK-NEXT:    [[IV_INT_ZEXT:%.*]] = zext i16 [[IV_INT]] to i64
; CHECK-NEXT:    [[ARRAYIDX:%.*]] = getelementptr inbounds [16777219 x i32], ptr @array, i64 0, i64 [[IV_INT_ZEXT]]
; CHECK-NEXT:    store i32 [[IV_INT_ZEXT1]], ptr [[ARRAYIDX]], align 4
; CHECK-NEXT:    [[DEC_INT]] = add nsw i16 [[IV_INT]], -1
; CHECK-NEXT:    [[CMP:%.*]] = icmp ugt i16 [[DEC_INT]], 0
; CHECK-NEXT:    br i1 [[CMP]], label [[FOR_BODY]], label [[CLEANUP:%.*]]
; CHECK:       cleanup:
; CHECK-NEXT:    ret void
;
entry:
  br label %for.body

for.body:                                         ; preds = %for.body, %entry
  %iv.int = phi i16 [ 100, %entry ], [ %dec.int, %for.body ]
  %indvar.conv = sitofp i16 %iv.int to float
  %idxprom32 = fptoui float %indvar.conv to i32
  %idxprom64 = fptoui float %indvar.conv to i64
  %arrayidx = getelementptr inbounds [16777219 x i32], ptr @array, i64 0, i64 %idxprom64
  store i32 %idxprom32, ptr %arrayidx, align 4
  %dec.int = add nsw i16 %iv.int, -1
  %cmp = icmp ugt i16 %dec.int, 0
  br i1 %cmp, label %for.body, label %cleanup

cleanup:                                          ; preds = %for.body
  ret void
}

; Take care of the insertion point.
define void @sitofp_fptoui_range_zext_postinc() {
;
; CHECK-LABEL: @sitofp_fptoui_range_zext_postinc(
; CHECK-NEXT:  entry:
; CHECK-NEXT:    br label [[FOR_BODY:%.*]]
; CHECK:       for.body:
; CHECK-NEXT:    [[IV_INT:%.*]] = phi i16 [ 100, [[ENTRY:%.*]] ], [ [[INC_INT:%.*]], [[FOR_BODY]] ]
; CHECK-NEXT:    [[INC_INT]] = add nuw nsw i16 [[IV_INT]], 2
; CHECK-NEXT:    [[INC_INT_ZEXT1:%.*]] = zext i16 [[INC_INT]] to i32
; CHECK-NEXT:    [[INC_INT_ZEXT:%.*]] = zext i16 [[INC_INT]] to i64
; CHECK-NEXT:    [[ARRAYIDX:%.*]] = getelementptr inbounds [16777219 x i32], ptr @array, i64 0, i64 [[INC_INT_ZEXT]]
; CHECK-NEXT:    store i32 [[INC_INT_ZEXT1]], ptr [[ARRAYIDX]], align 4
; CHECK-NEXT:    [[CMP:%.*]] = icmp ult i16 [[INC_INT]], 200
; CHECK-NEXT:    br i1 [[CMP]], label [[FOR_BODY]], label [[CLEANUP:%.*]]
; CHECK:       cleanup:
; CHECK-NEXT:    ret void
;
entry:
  br label %for.body

for.body:                                         ; preds = %for.body, %entry
  %iv.int = phi i16 [ 100, %entry ], [ %inc.int, %for.body ]
  %inc.int = add nsw i16 %iv.int, 2
  %indvar.conv = sitofp i16 %inc.int to float     ; The 'postinc IV' %inc.int passes to sitofp
  %idxprom32 = fptoui float %indvar.conv to i32
  %idxprom64 = fptoui float %indvar.conv to i64
  %arrayidx = getelementptr inbounds [16777219 x i32], ptr @array, i64 0, i64 %idxprom64
  store i32 %idxprom32, ptr %arrayidx, align 4
  %cmp = icmp ult i16 %inc.int, 200
  br i1 %cmp, label %for.body, label %cleanup

cleanup:                                          ; preds = %for.body
  ret void
}

; If one of them is unsigned, then we can use zext.
define void @uitofp_fptosi_range_zext() {
;
; CHECK-LABEL: @uitofp_fptosi_range_zext(
; CHECK-NEXT:  entry:
; CHECK-NEXT:    br label [[FOR_BODY:%.*]]
; CHECK:       for.body:
; CHECK-NEXT:    [[IV_INT:%.*]] = phi i16 [ 100, [[ENTRY:%.*]] ], [ [[INC_INT:%.*]], [[FOR_BODY]] ]
; CHECK-NEXT:    [[IV_INT_ZEXT1:%.*]] = zext i16 [[IV_INT]] to i32
; CHECK-NEXT:    [[IV_INT_ZEXT:%.*]] = zext i16 [[IV_INT]] to i64
; CHECK-NEXT:    [[ARRAYIDX:%.*]] = getelementptr inbounds [16777219 x i32], ptr @array, i64 0, i64 [[IV_INT_ZEXT]]
; CHECK-NEXT:    store i32 [[IV_INT_ZEXT1]], ptr [[ARRAYIDX]], align 4
; CHECK-NEXT:    [[INC_INT]] = add nuw nsw i16 [[IV_INT]], 2
; CHECK-NEXT:    [[CMP:%.*]] = icmp ult i16 [[INC_INT]], 200
; CHECK-NEXT:    br i1 [[CMP]], label [[FOR_BODY]], label [[CLEANUP:%.*]]
; CHECK:       cleanup:
; CHECK-NEXT:    ret void
;
entry:
  br label %for.body

for.body:                                         ; preds = %for.body, %entry
  %iv.int = phi i16 [ 100, %entry ], [ %inc.int, %for.body ]
  %indvar.conv = uitofp i16 %iv.int to float
  %idxprom32 = fptosi float %indvar.conv to i32
  %idxprom64 = fptosi float %indvar.conv to i64
  %arrayidx = getelementptr inbounds [16777219 x i32], ptr @array, i64 0, i64 %idxprom64
  store i32 %idxprom32, ptr %arrayidx, align 4
  %inc.int = add nsw i16 %iv.int, 2
  %cmp = icmp ult i16 %inc.int, 200
  br i1 %cmp, label %for.body, label %cleanup

cleanup:                                          ; preds = %for.body
  ret void
}


define void @sitofp_fptoui_range() {
; CHECK-LABEL: @sitofp_fptoui_range(
; CHECK-NEXT:  entry:
; CHECK-NEXT:    br label [[FOR_BODY:%.*]]
; CHECK:       for.body:
; CHECK-NEXT:    [[IV_INT:%.*]] = phi i32 [ 100, [[ENTRY:%.*]] ], [ [[DEC_INT:%.*]], [[FOR_BODY]] ]
; CHECK-NEXT:    [[IDXPROM:%.*]] = zext i32 [[IV_INT]] to i64
; CHECK-NEXT:    [[ARRAYIDX:%.*]] = getelementptr inbounds [16777219 x i32], ptr @array, i64 0, i64 [[IDXPROM]]
; CHECK-NEXT:    store i32 [[IV_INT]], ptr [[ARRAYIDX]], align 4
; CHECK-NEXT:    [[DEC_INT]] = add nsw i32 [[IV_INT]], -1
; CHECK-NEXT:    [[CMP:%.*]] = icmp samesign ugt i32 [[DEC_INT]], 0
; CHECK-NEXT:    br i1 [[CMP]], label [[FOR_BODY]], label [[CLEANUP:%.*]]
; CHECK:       cleanup:
; CHECK-NEXT:    ret void
;
entry:
  br label %for.body

for.body:                                         ; preds = %for.body, %entry
  %iv.int = phi i32 [ 100, %entry ], [ %dec.int, %for.body ]
  %indvar.conv = sitofp i32 %iv.int to float
  %conv = fptoui float %indvar.conv to i32
  %idxprom = zext i32 %conv to i64
  %arrayidx = getelementptr inbounds [16777219 x i32], ptr @array, i64 0, i64 %idxprom
  store i32 %conv, ptr %arrayidx, align 4
  %dec.int = add nsw i32 %iv.int, -1
  %cmp = icmp sgt i32 %dec.int, 0
  br i1 %cmp, label %for.body, label %cleanup

cleanup:                                          ; preds = %for.body
  ret void
}

; Range including negative value.
define void @sitofp_fptoui_range_with_negative () {
; CHECK-LABEL: @sitofp_fptoui_range_with_negative(
; CHECK-NEXT:  entry:
; CHECK-NEXT:    br label [[FOR_BODY:%.*]]
; CHECK:       for.body:
; CHECK-NEXT:    [[IV_INT:%.*]] = phi i32 [ 100, [[ENTRY:%.*]] ], [ [[DEC_INT:%.*]], [[FOR_BODY]] ]
; CHECK-NEXT:    [[IDXPROM:%.*]] = zext i32 [[IV_INT]] to i64
; CHECK-NEXT:    [[ARRAYIDX:%.*]] = getelementptr inbounds [16777219 x i32], ptr @array, i64 0, i64 [[IDXPROM]]
; CHECK-NEXT:    store i32 [[IV_INT]], ptr [[ARRAYIDX]], align 4
; CHECK-NEXT:    [[DEC_INT]] = add nsw i32 [[IV_INT]], -1
; CHECK-NEXT:    [[CMP:%.*]] = icmp sgt i32 [[DEC_INT]], -100
; CHECK-NEXT:    br i1 [[CMP]], label [[FOR_BODY]], label [[CLEANUP:%.*]]
; CHECK:       cleanup:
; CHECK-NEXT:    ret void
;
entry:
  br label %for.body

for.body:                                         ; preds = %for.body, %entry
  %iv.int = phi i32 [ 100, %entry ], [ %dec.int, %for.body ]
  %indvar.conv = sitofp i32 %iv.int to float
  %conv = fptoui float %indvar.conv to i32
  %idxprom = zext i32 %conv to i64
  %arrayidx = getelementptr inbounds [16777219 x i32], ptr @array, i64 0, i64 %idxprom
  store i32 %conv, ptr %arrayidx, align 4
  %dec.int = add nsw i32 %iv.int, -1
  %cmp = icmp sgt i32 %dec.int, -100
  br i1 %cmp, label %for.body, label %cleanup

cleanup:                                          ; preds = %for.body
  ret void
}

; https://godbolt.org/z/51MrqYjEf
define void @uitofp_fptoui_range () {
; CHECK-LABEL: @uitofp_fptoui_range(
; CHECK-NEXT:  entry:
; CHECK-NEXT:    br label [[FOR_BODY:%.*]]
; CHECK:       for.body:
; CHECK-NEXT:    [[IV_INT:%.*]] = phi i32 [ 100, [[ENTRY:%.*]] ], [ [[DEC_INT:%.*]], [[FOR_BODY]] ]
; CHECK-NEXT:    [[IDXPROM:%.*]] = zext i32 [[IV_INT]] to i64
; CHECK-NEXT:    [[ARRAYIDX:%.*]] = getelementptr inbounds [16777219 x i32], ptr @array, i64 0, i64 [[IDXPROM]]
; CHECK-NEXT:    store i32 [[IV_INT]], ptr [[ARRAYIDX]], align 4
; CHECK-NEXT:    [[DEC_INT]] = add nsw i32 [[IV_INT]], -1
; CHECK-NEXT:    [[CMP:%.*]] = icmp ugt i32 [[DEC_INT]], 3
; CHECK-NEXT:    br i1 [[CMP]], label [[FOR_BODY]], label [[CLEANUP:%.*]]
; CHECK:       cleanup:
; CHECK-NEXT:    ret void
;
entry:
  br label %for.body

for.body:                                         ; preds = %for.body, %entry
  %iv.int = phi i32 [ 100, %entry ], [ %dec.int, %for.body ]
  %indvar.conv = uitofp i32 %iv.int to float
  %conv = fptoui float %indvar.conv to i32
  %idxprom = zext i32 %conv to i64
  %arrayidx = getelementptr inbounds [16777219 x i32], ptr @array, i64 0, i64 %idxprom
  store i32 %conv, ptr %arrayidx, align 4
  %dec.int = add nsw i32 %iv.int, -1
  %cmp = icmp ugt i32 %dec.int, 3
  br i1 %cmp, label %for.body, label %cleanup

cleanup:                                          ; preds = %for.body
  ret void
}

define void @uitofp_fptoui_range_with_negative() {
; CHECK-LABEL: @uitofp_fptoui_range_with_negative(
; CHECK-NEXT:  entry:
; CHECK-NEXT:    br label [[FOR_BODY:%.*]]
; CHECK:       for.body:
; CHECK-NEXT:    store i32 100, ptr getelementptr inbounds nuw (i8, ptr @array, i64 400), align 4
; CHECK-NEXT:    br i1 false, label [[FOR_BODY]], label [[CLEANUP:%.*]]
; CHECK:       cleanup:
; CHECK-NEXT:    ret void
;
entry:
  br label %for.body

for.body:                                         ; preds = %for.body, %entry
  %iv.int = phi i32 [ 100, %entry ], [ %dec.int, %for.body ]
  %indvar.conv = uitofp i32 %iv.int to float
  %conv = fptoui float %indvar.conv to i32
  %idxprom = zext i32 %conv to i64
  %arrayidx = getelementptr inbounds [16777219 x i32], ptr @array, i64 0, i64 %idxprom
  store i32 %conv, ptr %arrayidx, align 4
  %dec.int = add nsw i32 %iv.int, -1
  %cmp = icmp ugt i32 %dec.int, -100
  br i1 %cmp, label %for.body, label %cleanup

cleanup:                                          ; preds = %for.body
  ret void
}

define void @uitofp_fptosi_range () {
; CHECK-LABEL: @uitofp_fptosi_range(
; CHECK-NEXT:  entry:
; CHECK-NEXT:    br label [[FOR_BODY:%.*]]
; CHECK:       for.body:
; CHECK-NEXT:    [[IV_INT:%.*]] = phi i32 [ 100, [[ENTRY:%.*]] ], [ [[DEC_INT:%.*]], [[FOR_BODY]] ]
; CHECK-NEXT:    [[IDXPROM:%.*]] = sext i32 [[IV_INT]] to i64
; CHECK-NEXT:    [[ARRAYIDX:%.*]] = getelementptr inbounds [16777219 x i32], ptr @array, i64 0, i64 [[IDXPROM]]
; CHECK-NEXT:    store i32 [[IV_INT]], ptr [[ARRAYIDX]], align 4
; CHECK-NEXT:    [[DEC_INT]] = add nsw i32 [[IV_INT]], -1
; CHECK-NEXT:    [[CMP:%.*]] = icmp ugt i32 [[DEC_INT]], 3
; CHECK-NEXT:    br i1 [[CMP]], label [[FOR_BODY]], label [[CLEANUP:%.*]]
; CHECK:       cleanup:
; CHECK-NEXT:    ret void
;
entry:
  br label %for.body

for.body:                                         ; preds = %for.body, %entry
  %iv.int = phi i32 [ 100, %entry ], [ %dec.int, %for.body ]
  %indvar.conv = uitofp i32 %iv.int to float
  %conv = fptosi float %indvar.conv to i32
  %idxprom = sext i32 %conv to i64
  %arrayidx = getelementptr inbounds [16777219 x i32], ptr @array, i64 0, i64 %idxprom
  store i32 %conv, ptr %arrayidx, align 4
  %dec.int = add nsw i32 %iv.int, -1
  %cmp = icmp ugt i32 %dec.int, 3
  br i1 %cmp, label %for.body, label %cleanup

cleanup:                                          ; preds = %for.body
  ret void
}

define void @uitofp_fptosi_range_with_negative () {
; CHECK-LABEL: @uitofp_fptosi_range_with_negative(
; CHECK-NEXT:  entry:
; CHECK-NEXT:    br label [[FOR_BODY:%.*]]
; CHECK:       for.body:
; CHECK-NEXT:    store i32 100, ptr getelementptr inbounds nuw (i8, ptr @array, i64 400), align 4
; CHECK-NEXT:    br i1 false, label [[FOR_BODY]], label [[CLEANUP:%.*]]
; CHECK:       cleanup:
; CHECK-NEXT:    ret void
;
entry:
  br label %for.body

for.body:                                         ; preds = %for.body, %entry
  %iv.int = phi i32 [ 100, %entry ], [ %dec.int, %for.body ]
  %indvar.conv = uitofp i32 %iv.int to float
  %conv = fptosi float %indvar.conv to i32
  %idxprom = sext i32 %conv to i64
  %arrayidx = getelementptr inbounds [16777219 x i32], ptr @array, i64 0, i64 %idxprom
  store i32 %conv, ptr %arrayidx, align 4
  %dec.int = add nsw i32 %iv.int, -1
  %cmp = icmp ugt i32 %dec.int, -100
  br i1 %cmp, label %for.body, label %cleanup

cleanup:                                          ; preds = %for.body
  ret void
}

