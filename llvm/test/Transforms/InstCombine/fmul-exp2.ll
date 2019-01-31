; NOTE: Assertions have been autogenerated by utils/update_test_checks.py
; RUN: opt -S -instcombine < %s | FileCheck %s

declare double @llvm.exp2.f64(double) nounwind readnone speculatable
declare void @use(double)

; exp2(a) * exp2(b) no reassoc flags
define double @exp2_a_exp2_b(double %a, double %b) {
; CHECK-LABEL: @exp2_a_exp2_b(
; CHECK-NEXT:    [[TMP:%.*]] = call double @llvm.exp2.f64(double [[A:%.*]])
; CHECK-NEXT:    [[TMP1:%.*]] = call double @llvm.exp2.f64(double [[B:%.*]])
; CHECK-NEXT:    [[MUL:%.*]] = fmul double [[TMP]], [[TMP1]]
; CHECK-NEXT:    ret double [[MUL]]
;
  %tmp = call double @llvm.exp2.f64(double %a)
  %tmp1 = call double @llvm.exp2.f64(double %b)
  %mul = fmul double %tmp, %tmp1
  ret double %mul
}

; exp2(a) * exp2(b) reassoc, multiple uses
define double @exp2_a_exp2_b_multiple_uses(double %a, double %b) {
; CHECK-LABEL: @exp2_a_exp2_b_multiple_uses(
; CHECK-NEXT:    [[TMP1:%.*]] = call double @llvm.exp2.f64(double [[B:%.*]])
; CHECK-NEXT:    [[TMP:%.*]] = fadd reassoc double [[A:%.*]], [[B]]
; CHECK-NEXT:    [[TMP2:%.*]] = call reassoc double @llvm.exp2.f64(double [[TMP]])
; CHECK-NEXT:    call void @use(double [[TMP1]])
; CHECK-NEXT:    ret double [[TMP2]]
;
  %tmp = call double @llvm.exp2.f64(double %a)
  %tmp1 = call double @llvm.exp2.f64(double %b)
  %mul = fmul reassoc double %tmp, %tmp1
  call void @use(double %tmp1)
  ret double %mul
}

; exp2(a) * exp2(b) reassoc, both with multiple uses
define double @exp2_a_exp2_b_multiple_uses_both(double %a, double %b) {
; CHECK-LABEL: @exp2_a_exp2_b_multiple_uses_both(
; CHECK-NEXT:    [[TMP:%.*]] = call double @llvm.exp2.f64(double [[A:%.*]])
; CHECK-NEXT:    [[TMP1:%.*]] = call double @llvm.exp2.f64(double [[B:%.*]])
; CHECK-NEXT:    [[MUL:%.*]] = fmul reassoc double [[TMP]], [[TMP1]]
; CHECK-NEXT:    call void @use(double [[TMP]])
; CHECK-NEXT:    call void @use(double [[TMP1]])
; CHECK-NEXT:    ret double [[MUL]]
;
  %tmp = call double @llvm.exp2.f64(double %a)
  %tmp1 = call double @llvm.exp2.f64(double %b)
  %mul = fmul reassoc double %tmp, %tmp1
  call void @use(double %tmp)
  call void @use(double %tmp1)
  ret double %mul
}

; exp2(a) * exp2(b) => exp2(a+b) with reassoc
define double @exp2_a_exp2_b_reassoc(double %a, double %b) {
; CHECK-LABEL: @exp2_a_exp2_b_reassoc(
; CHECK-NEXT:    [[TMP:%.*]] = fadd reassoc double [[A:%.*]], [[B:%.*]]
; CHECK-NEXT:    [[TMP1:%.*]] = call reassoc double @llvm.exp2.f64(double [[TMP]])
; CHECK-NEXT:    ret double [[TMP1]]
;
  %tmp = call double @llvm.exp2.f64(double %a)
  %tmp1 = call double @llvm.exp2.f64(double %b)
  %mul = fmul reassoc double %tmp, %tmp1
  ret double %mul
}

; exp2(a) * exp2(b) * exp2(c) * exp2(d) => exp2(a+b+c+d) with reassoc
define double @exp2_a_exp2_b_exp2_c_exp2_d(double %a, double %b, double %c, double %d) {
; CHECK-LABEL: @exp2_a_exp2_b_exp2_c_exp2_d(
; CHECK-NEXT:    [[TMP:%.*]] = fadd reassoc double [[A:%.*]], [[B:%.*]]
; CHECK-NEXT:    [[TMP1:%.*]] = fadd reassoc double [[TMP]], [[C:%.*]]
; CHECK-NEXT:    [[TMP2:%.*]] = fadd reassoc double [[TMP1]], [[D:%.*]]
; CHECK-NEXT:    [[TMP3:%.*]] = call reassoc double @llvm.exp2.f64(double [[TMP2]])
; CHECK-NEXT:    ret double [[TMP3]]
;
  %tmp = call double @llvm.exp2.f64(double %a)
  %tmp1 = call double @llvm.exp2.f64(double %b)
  %mul = fmul reassoc double %tmp, %tmp1
  %tmp2 = call double @llvm.exp2.f64(double %c)
  %mul1 = fmul reassoc double %mul, %tmp2
  %tmp3 = call double @llvm.exp2.f64(double %d)
  %mul2 = fmul reassoc double %mul1, %tmp3
  ret double %mul2
}
