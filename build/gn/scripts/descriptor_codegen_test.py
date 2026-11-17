"""Real-generator regression tests.

Before rebuilding, save the old runtime_feature_flags.{h,cpp} and
control_board_proto.{h,cpp} in --reference-dir. Build the two generated
targets with pch_mode="global", then run:

  python3 build/gn/scripts/descriptor_codegen_test.py \
      --build-dir out/gn --reference-dir /path/to/old/outputs

The suite invokes the built protoc and generators. It does not build anything.
"""

import argparse
from pathlib import Path
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[3]
BUILD = None
REFERENCE = None

FEATURE_PROTO = """
syntax = "proto2";
package NKikimrConfig;
import "google/protobuf/descriptor.proto";
extend google.protobuf.FieldOptions {
  optional bool RequireRestart = 61000 [default = false];
}
message TFeatureFlags {
  optional bool DefaultOn = 1 [default = true];
  optional bool Restart = 2 [(RequireRestart) = true];
  optional bool ExplicitRuntime = 3 [(RequireRestart) = false];
  optional string Ignored = 4;
  %s
}
""" % "\n".join("optional bool Pad%d = %d;" % (i, i + 5) for i in range(30))

CONFIG_PROTO = """
syntax = "proto2";
package NKikimrConfig;
import "google/protobuf/descriptor.proto";
message TImmediateControlOptions {
  optional int64 MinValue = 2;
  optional uint64 MaxValue = 3;
  optional uint64 DefaultValue = 4;
}
extend google.protobuf.FieldOptions {
  optional TImmediateControlOptions ControlOptions = 61001;
}
message TImmediateControlsConfig {
  message TDataShardControls {
    optional uint64 Absent = 1;
    optional uint64 Zero = 2 [(ControlOptions) = {
      MinValue: 0 MaxValue: 0 DefaultValue: 0
    }];
    optional uint64 Signed = 3 [(ControlOptions) = {
      MinValue: -7 MaxValue: 9 DefaultValue: 3
    }];
    optional uint64 Empty = 4 [(ControlOptions) = {}];
  }
  message TGRpcControls {
    message TRequestConfig {
      optional uint64 Timeout = 1 [(ControlOptions) = {
        MinValue: 0 MaxValue: 50 DefaultValue: 1
      }];
    }
    map<string, TRequestConfig> RequestConfigs = 1;
  }
  optional TDataShardControls DataShardControls = 1;
  optional TGRpcControls GRpcControls = 2;
}
"""

TABLET_PROTO = """
syntax = "proto2";
package NKikimrTabletBase;
import "google/protobuf/descriptor.proto";
message TTabletTypes {
  enum EType {
    First = 1;
    Second = 2;
    UserTypeStart = 100;
    User = 101;
  }
}
"""

FEATURE_TEMPLATE = (
    "{% for field in fields %}{{ field.name }}|"
    "{% if field.is_runtime %}runtime{% else %}restart{% endif %}|"
    "{{ field.default_value }}|{{ field.slot.index }}\n{% endfor %}"
)

CONTROL_TEMPLATE = (
    "{% for control in controls %}{{ control.name }}|"
    "{% if control.has_full_path %}{{ control.full_path }}{% endif %}|"
    "{% if control.has_html_name %}{{ control.html_name }}{% endif %}|"
    "{% if control.min_value is defined %}{{ control.min_value }}{% else %}absent{% endif %}|"
    "{% if control.max_value is defined %}{{ control.max_value }}{% else %}absent{% endif %}|"
    "{% if control.default_value is defined %}{{ control.default_value }}{% else %}absent{% endif %}"
    "\n{% endfor %}"
)


class DescriptorCodegenTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix="descriptor-codegen-")
        self.addCleanup(self.tmp.cleanup)
        self.work = Path(self.tmp.name)
        self.tools = BUILD / "clang_no_global_pch"
        self.feature = self.tools / "ydb_core_base_generated_codegen"
        self.control = self.tools / "ydb_core_control_lib_generated_codegen"

    def run_command(self, args, success=True):
        result = subprocess.run([str(a) for a in args], cwd=ROOT,
                                capture_output=True, text=True)
        if success:
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        else:
            self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
        return result

    def descriptor(self, name, source):
        proto = self.work / (name + ".proto")
        proto.write_text(source)
        output = self.work / (name + ".desc")
        self.run_command([self.tools / "protoc", "-I" + str(self.work),
                          "-I" + str(ROOT / "contrib/libs/protobuf/src"),
                          "--include_imports", "--descriptor_set_out=" + str(output), proto])
        return output

    def generate(self, executable, descriptors, template, success=True):
        source = self.work / "template.in"
        output = self.work / "generated.txt"
        source.write_text(template)
        result = self.run_command([executable] + list(descriptors) + [source, output],
                                  success=success)
        return output.read_text() if success else result

    def test_feature_flags_options_and_slot_boundary(self):
        descriptor = self.descriptor("feature", FEATURE_PROTO)
        lines = self.generate(self.feature, [descriptor], FEATURE_TEMPLATE).splitlines()
        self.assertEqual(len(lines), 33)
        self.assertEqual(lines[:3], ["DefaultOn|runtime|2|0", "Restart|restart|0|0",
                                    "ExplicitRuntime|runtime|0|0"])
        self.assertEqual(lines[-2:], ["Pad28|runtime|0|0", "Pad29|runtime|0|1"])
        self.assertFalse(any("Ignored" in line for line in lines))

    def test_extension_default_and_explicit_false(self):
        source = FEATURE_PROTO.replace(
            "RequireRestart = 61000 [default = false]",
            "RequireRestart = 61000 [default = true]")
        descriptor = self.descriptor("feature", source)
        lines = self.generate(self.feature, [descriptor], FEATURE_TEMPLATE).splitlines()
        self.assertEqual(lines[:3], ["DefaultOn|restart|2|0", "Restart|restart|0|0",
                                    "ExplicitRuntime|runtime|0|0"])

    def test_control_options_presence_signed_min_and_tablet_boundary(self):
        config = self.descriptor("config", CONFIG_PROTO)
        tablet = self.descriptor("tablet", TABLET_PROTO)
        # Both sets contain descriptor.proto; overlapping imports must be accepted.
        lines = self.generate(self.control, [config, tablet], CONTROL_TEMPLATE).splitlines()
        self.assertIn("Absent|DataShardControls.Absent|DataShardControls.Absent|absent|absent|absent", lines)
        self.assertIn("Empty|DataShardControls.Empty|DataShardControls.Empty|absent|absent|absent", lines)
        self.assertIn("Zero|DataShardControls.Zero|DataShardControls.Zero|0|0|0", lines)
        self.assertIn("Signed|DataShardControls.Signed|DataShardControls.Signed|-7|9|3", lines)
        self.assertIn("Timeout|GRpcControls.RequestConfigs.FooBar.Timeout|GRpcControls.RequestConfigs.FooBar.Timeout|0|50|1", lines)
        self.assertTrue(any("First_LogFlushDelayOverrideUsec" in line for line in lines))
        self.assertTrue(any("Second_LogFlushDelayOverrideUsec" in line for line in lines))
        self.assertFalse(any("User_LogFlushDelayOverrideUsec" in line or
                             "UserTypeStart_LogFlushDelayOverrideUsec" in line for line in lines))
        # Loading order is immaterial, including which set supplies shared imports.
        reversed_lines = self.generate(self.control, [tablet, config], CONTROL_TEMPLATE).splitlines()
        self.assertEqual(reversed_lines, lines)

    def test_conflicting_descriptor_sets_fail(self):
        config = self.descriptor("config", CONFIG_PROTO)
        saved = self.work / "original.desc"
        saved.write_bytes(config.read_bytes())
        changed = self.descriptor("config", CONFIG_PROTO.replace("MaxValue = 3", "MaxValue = 30"))
        result = self.generate(self.control, [saved, changed], CONTROL_TEMPLATE, success=False)
        self.assertIn("Conflicting file descriptors: config.proto", result.stderr)

    def test_missing_descriptors_fail(self):
        result = self.generate(self.feature, [self.work / "missing.desc"],
                               FEATURE_TEMPLATE, success=False)
        self.assertIn("Cannot read descriptor set", result.stderr)
        corrupt = self.work / "corrupt.desc"
        corrupt.write_bytes(b"\xff")
        result = self.generate(self.feature, [corrupt], FEATURE_TEMPLATE, success=False)
        self.assertIn("Cannot read descriptor set", result.stderr)
        unrelated = self.descriptor("tablet", TABLET_PROTO)
        result = self.generate(self.feature, [unrelated], FEATURE_TEMPLATE, success=False)
        self.assertIn("Missing message descriptor: NKikimrConfig.TFeatureFlags", result.stderr)

    def test_real_outputs_match_reference(self):
        cases = [
            (self.feature, "ydb/core/base/generated", "runtime_feature_flags",
             ["feature_flags_descriptor"]),
            (self.control, "ydb/core/control/lib/generated", "control_board_proto",
             ["config_descriptor", "tablet_descriptor"]),
        ]
        for executable, directory, stem, descriptors in cases:
            args = [executable] + [BUILD / "gen" / directory / (d + ".desc") for d in descriptors]
            for extension in ("h", "cpp"):
                filename = stem + "." + extension
                output = self.work / filename
                args += [ROOT / directory / (filename + ".in"), output]
            self.run_command(args)
            for extension in ("h", "cpp"):
                filename = stem + "." + extension
                with self.subTest(file=filename):
                    expected = (REFERENCE / filename).read_bytes()
                    self.assertEqual((self.work / filename).read_bytes(), expected)
                    self.assertEqual((BUILD / "gen" / directory / filename).read_bytes(), expected)

    def test_auxiliary_graph_has_no_core_protos_or_grpc_runtime(self):
        graph = (self.tools / "toolchain.ninja").read_text()
        self.assertNotIn("subninja clang_no_global_pch/obj/ydb/core/protos/protos.ninja", graph)
        self.assertNotIn("subninja clang_no_global_pch/obj/contrib/libs/grpc/grpc.ninja", graph)

    def test_descriptor_depfiles_include_imports(self):
        cases = [
            ("gen/ydb/core/base/generated/feature_flags_descriptor.desc",
             "ydb/core/protos/feature_flags.proto"),
            ("gen/ydb/core/control/lib/generated/config_descriptor.desc",
             "ydb/core/protos/feature_flags.proto"),
            ("gen/ydb/core/control/lib/generated/tablet_descriptor.desc",
             "ydb/core/protos/base.proto"),
        ]
        for output, dependency in cases:
            with self.subTest(output=output):
                # GN action depfiles are read directly by Ninja (no deps=gcc);
                # they are not stored in the .ninja_deps database.
                depfile = (BUILD / (output + ".d")).read_text()
                self.assertIn(output, depfile)
                self.assertIn(dependency, depfile)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, required=True)
    parser.add_argument("--reference-dir", type=Path, required=True)
    args, unittest_args = parser.parse_known_args()
    BUILD = args.build_dir.resolve()
    REFERENCE = args.reference_dir.resolve()
    unittest.main(argv=[__file__] + unittest_args)
