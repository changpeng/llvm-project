// RUN: %clang_cc1 -std=c++20 -Wno-everything -Wunsafe-buffer-usage \
// RUN:            -fsafe-buffer-usage-suggestions \
// RUN:            -verify %s

// CHECK-NOT: [-Wunsafe-buffer-usage]


void foo(unsigned idx) {
  int buffer[10];         // expected-warning{{'buffer' is an unsafe buffer that does not perform bounds checks}}
                          // expected-note@-1{{change type of 'buffer' to 'std::array' to label it for hardening}}
  buffer[idx] = 0;        // expected-note{{used in buffer access here}}
}

int global_buffer[10];    // expected-warning{{'global_buffer' is an unsafe buffer that does not perform bounds checks}}
void foo2(unsigned idx) {
  global_buffer[idx] = 0;        // expected-note{{used in buffer access here}}
}

struct Foo {
  int member_buffer[10];
};
void foo2(Foo& f, unsigned idx) {
  f.member_buffer[idx] = 0; // expected-warning{{unsafe buffer access}}
}

void constant_idx_safe(unsigned idx) {
  int buffer[10];
  buffer[9] = 0;
}

void constant_idx_safe0(unsigned idx) {
  int buffer[10];
  buffer[0] = 0;
}

void constant_idx_unsafe(unsigned idx) {
  int buffer[10];       // expected-warning{{'buffer' is an unsafe buffer that does not perform bounds checks}}
                        // expected-note@-1{{change type of 'buffer' to 'std::array' to label it for hardening}}
  buffer[10] = 0;       // expected-note{{used in buffer access here}}
}

void constant_id_string(unsigned idx) {
  char safe_char = "abc"[1]; // no-warning
  safe_char = ""[0];
  safe_char = "\0"[0];
 
  char abcd[5] = "abc";
  abcd[2]; // no-warning

  char unsafe_char = "abc"[3];
  unsafe_char = "abc"[-1]; //expected-warning{{unsafe buffer access}}
  unsafe_char = ""[1]; //expected-warning{{unsafe buffer access}} 
  unsafe_char = ""[idx]; //expected-warning{{unsafe buffer access}}
}

typedef float Float4x4[4][4];

// expected-warning@+1 {{'matrix' is an unsafe buffer that does not perform bounds checks}}
float two_dimension_array(Float4x4& matrix, unsigned idx) {
  // expected-warning@+1{{unsafe buffer access}}
  float a = matrix[0][4];

  a = matrix[0][3];

  // expected-note@+1{{used in buffer access here}}
  a = matrix[4][0];

  a = matrix[idx][0]; // expected-note{{used in buffer access here}}

  a = matrix[0][idx]; //expected-warning{{unsafe buffer access}}

  a = matrix[idx][idx]; //expected-warning{{unsafe buffer access}} // expected-note{{used in buffer access here}}

  return matrix[1][1];
}

typedef float Float2x3x4[2][3][4];
float multi_dimension_array(Float2x3x4& matrix) {
  float *f = matrix[0][2];
  return matrix[1][2][3];
}

char array_strings[][11] = {
  "Apple", "Banana", "Cherry", "Date", "Elderberry"
};

char array_string[] = "123456";

char access_strings() {
  char c = array_strings[0][4];
  c = array_strings[3][10];
  c = array_string[5];
  return c;
}

struct T {
  int array[10];
};

const int index = 1;

constexpr int get_const(int x) {
  if(x < 3)
    return ++x;
  else
    return x + 5;
};

void array_indexed_const_expr(unsigned idx) {
  // expected-note@+2 {{change type of 'arr' to 'std::array' to label it for hardening}}
  // expected-warning@+1{{'arr' is an unsafe buffer that does not perform bounds checks}}
  int arr[10];
  arr[sizeof(int)] = 5;

  int array[sizeof(T)];
  array[sizeof(int)] = 5;
  array[sizeof(T) -1 ] = 3;

  int k = arr[6 & 5];
  k = arr[2 << index];
  k = arr[8 << index]; // expected-note {{used in buffer access here}}
  k = arr[16 >> 1];
  k = arr[get_const(index)];
  k = arr[get_const(5)]; // expected-note {{used in buffer access here}}
  k = arr[get_const(4)];
}

template<unsigned length>
consteval bool isNullTerminated(const char (&literal)[length])
{
  return literal[length - 1] == '\0';
}

template <typename T, unsigned M, unsigned N>
T access2DArray(const T (&arr)[M][N]) {
  return arr[M-1][N-1];
}

template<unsigned idx>
constexpr int access_elements() {
  int arr[idx + 20];
  return arr[idx + 1];
}

// Test array accesses where const sized arrays are accessed safely with indices
// that evaluate to a const values and depend on template arguments.
void test_template_methods()
{
  constexpr char arr[] = "Good Morning!"; // = {'a', 'b', 'c', 'd', 'e'};
  isNullTerminated(arr);
  isNullTerminated("");
  auto _ = isNullTerminated("hello world\n");
  access_elements<5>();

  int arr1[3][4] = {
        {1, 2, 3, 4},
        {5, 6, 7, 8},
        {9, 10, 11, 12}
    };
  access2DArray(arr1);
}
