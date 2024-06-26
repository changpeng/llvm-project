! UNSUPPORTED: system-windows
! Marking as unsupported due to suspected long runtime on Windows
!RUN: %python %S/../test_errors.py %s %flang -fopenmp
! OpenMP Version 4.5
! 2.7.1 Schedule Clause
program omp_doSchedule
  integer :: i,n
  real ::  a(100), y(100), z(100)
  integer,parameter :: b = 10
  integer,parameter :: c = 11
  !ERROR: The chunk size of the SCHEDULE clause must be a positive integer expression
  !$omp do schedule(static,b-c)
  do i=2,n+1
    y(i) = z(i-1) + a(i)
  end do
  !$omp end do
end program omp_doSchedule
