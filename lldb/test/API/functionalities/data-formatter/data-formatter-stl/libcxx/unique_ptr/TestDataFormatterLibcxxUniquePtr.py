"""
Test lldb data formatter for libc++ std::unique_ptr.
"""


import lldb
from lldbsuite.test.decorators import *
from lldbsuite.test.lldbtest import *
from lldbsuite.test import lldbutil


class TestCase(TestBase):
    def make_expected_type(self, pointee_type: str, qualifiers: str = "") -> str:
        if qualifiers:
            qualifiers = " " + qualifiers

        if self.expectedCompiler(["clang"]) and self.expectedCompilerVersion(
            [">", "16.0"]
        ):
            return f"std::unique_ptr<{pointee_type}>{qualifiers}"
        else:
            return f"std::unique_ptr<{pointee_type}, std::default_delete<{pointee_type}> >{qualifiers}"

    def make_expected_basic_string_ptr(self) -> str:
        if self.expectedCompiler(["clang"]) and self.expectedCompilerVersion(
            [">", "16.0"]
        ):
            return f"std::unique_ptr<std::string>"
        else:
            return (
                "std::unique_ptr<std::basic_string<char, std::char_traits<char>, std::allocator<char> >, "
                "std::default_delete<std::basic_string<char, std::char_traits<char>, std::allocator<char> > > >"
            )

    @add_test_categories(["libc++"])
    def test_unique_ptr_variables(self):
        """Test `frame variable` output for `std::unique_ptr` types."""
        self.build()

        lldbutil.run_to_source_breakpoint(
            self, "// break here", lldb.SBFileSpec("main.cpp")
        )

        valobj = self.expect_var_path(
            "up_empty",
            type=self.make_expected_type("int"),
            summary="nullptr",
            children=[ValueCheck(name="pointer")],
        )
        self.assertEqual(
            valobj.child[0].GetValueAsUnsigned(lldb.LLDB_INVALID_ADDRESS), 0
        )

        self.expect(
            "frame variable *up_empty", substrs=["(int) *up_empty = <parent is NULL>"]
        )

        valobj = self.expect_var_path(
            "up_int",
            type=self.make_expected_type("int"),
            summary="10",
            children=[ValueCheck(name="pointer")],
        )
        self.assertNotEqual(valobj.child[0].unsigned, 0)

        valobj = self.expect_var_path(
            "up_int_ref",
            type=self.make_expected_type("int", qualifiers="&"),
            summary="10",
            children=[ValueCheck(name="pointer")],
        )
        self.assertNotEqual(valobj.child[0].unsigned, 0)

        valobj = self.expect_var_path(
            "up_int_ref_ref",
            type=self.make_expected_type("int", qualifiers="&&"),
            summary="10",
            children=[ValueCheck(name="pointer")],
        )
        self.assertNotEqual(valobj.child[0].unsigned, 0)

        valobj = self.expect_var_path(
            "up_str",
            type=self.make_expected_basic_string_ptr(),
            summary='"hello"',
            children=[ValueCheck(name="pointer", summary='"hello"')],
        )

        valobj = self.expect_var_path("up_user", type=self.make_expected_type("User"))
        self.assertRegex(valobj.summary, "^User @ 0x0*[1-9a-f][0-9a-f]+$")
        self.assertNotEqual(valobj.child[0].unsigned, 0)

        valobj = self.expect_var_path(
            "*up_user",
            type="User",
            children=[
                ValueCheck(name="id", value="30"),
                ValueCheck(name="name", summary='"steph"'),
            ],
        )
        self.assertEqual(str(valobj), '(User) *pointer = (id = 30, name = "steph")')

        valobj = self.expect_var_path(
            "up_non_empty_deleter",
            type="std::unique_ptr<int, NonEmptyIntDeleter>",
            summary="1234",
            children=[
                ValueCheck(name="pointer"),
                ValueCheck(
                    name="deleter", children=[ValueCheck(name="dummy_", value="9999")]
                ),
            ],
        )
        self.assertNotEqual(valobj.child[0].unsigned, 0)

        self.expect_var_path("up_user->id", type="int", value="30")
        self.expect_var_path("up_user->name", type="std::string", summary='"steph"')

        self.runCmd("settings set target.experimental.use-DIL true")
        self.expect_var_path("ptr_node->value", value="1")
        self.expect_var_path("ptr_node->next->value", value="2")
        self.expect_var_path("(*ptr_node).value", value="1")
        self.expect_var_path("(*(*ptr_node).next).value", value="2")
