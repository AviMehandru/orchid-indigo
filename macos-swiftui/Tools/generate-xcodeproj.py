#!/usr/bin/env python3
"""Generate YtdlMac.xcodeproj from what is actually in Sources/ and Tests/.

WHY THIS EXISTS. An .xcodeproj is the build system for a macOS app, and its
project.pbxproj is a 700-line file in which every source file appears three
times under a 24-character identifier. Hand-editing it to add a file is how a
project ends up with a source that is in the repository and not in the build --
which compiles, ships, and is missing a feature.

So the project is GENERATED from the file system: every .swift under Sources/
goes into the app target, every .swift under Tests/ into the test target, and
the identifiers are derived from the paths so that regenerating produces the
same file rather than a diff of shuffled hex.

    python3 Tools/generate-xcodeproj.py

Run it after adding, renaming or deleting a source file, and commit the result.
Nothing else in the project should ever be edited by hand either -- change this
script and re-run it.

The generated project has exactly two targets and no dependencies of any kind:
SwiftUI, AppKit, AVKit, ImageIO and CryptoKit are auto-linked by the compiler
from the import statements, which is why there is no framework list here.
"""

from __future__ import annotations

import hashlib
import os
import pathlib
import sys

HERE = pathlib.Path(__file__).resolve().parent
ROOT = HERE.parent                      # macos-swiftui/
REPO = ROOT.parent                      # the repository root

PROJECT_NAME = "YtdlMac"
APP_TARGET = "ytdl-macos"
TEST_TARGET = "ytdl-macosTests"
APP_MODULE = "YtdlMac"
BUNDLE_ID = "io.github.avimehandru.YtdlMac"
DEPLOYMENT_TARGET = "14.0"              # Sonoma: ContentUnavailableView, and no shims
SWIFT_VERSION = "5.0"
MARKETING_VERSION = "0.1.0"


def uid(name: str) -> str:
    """A stable 24-hex-character identifier for a name.

    Xcode generates these randomly; deriving them from the path means the file
    regenerates byte for byte instead of producing a diff in which every object
    moved.
    """
    return hashlib.md5(name.encode()).hexdigest()[:24].upper()


def swift_files(directory: pathlib.Path) -> list[pathlib.Path]:
    return sorted(p.relative_to(ROOT) for p in directory.rglob("*.swift"))


def quote(value: str) -> str:
    """Quote a pbxproj value if it is not a bare identifier."""
    if value and all(c.isalnum() or c in "_./" for c in value):
        return value
    return '"' + value.replace("\\", "\\\\").replace('"', '\\"') + '"'


def settings_block(pairs: dict[str, str], indent: str) -> str:
    out = []
    for key in sorted(pairs):
        out.append(f"{indent}{key} = {pairs[key]};")
    return "\n".join(out)


def main() -> int:
    app_sources = swift_files(ROOT / "Sources")
    test_sources = swift_files(ROOT / "Tests")
    if not app_sources or not test_sources:
        print("no sources found; run this from a checkout", file=sys.stderr)
        return 1

    # --- identifiers ----------------------------------------------------
    ids = {
        "project": uid("project"),
        "app_target": uid("target:app"),
        "test_target": uid("target:tests"),
        "main_group": uid("group:main"),
        "products_group": uid("group:products"),
        "app_product": uid("product:app"),
        "test_product": uid("product:tests"),
        "app_sources_phase": uid("phase:app:sources"),
        "app_frameworks_phase": uid("phase:app:frameworks"),
        "app_resources_phase": uid("phase:app:resources"),
        "test_sources_phase": uid("phase:test:sources"),
        "test_frameworks_phase": uid("phase:test:frameworks"),
        "test_resources_phase": uid("phase:test:resources"),
        "project_config_list": uid("configlist:project"),
        "app_config_list": uid("configlist:app"),
        "test_config_list": uid("configlist:tests"),
        "project_debug": uid("config:project:debug"),
        "project_release": uid("config:project:release"),
        "app_debug": uid("config:app:debug"),
        "app_release": uid("config:app:release"),
        "test_debug": uid("config:tests:debug"),
        "test_release": uid("config:tests:release"),
        "dependency": uid("dependency:tests->app"),
        "container_proxy": uid("proxy:tests->app"),
        "cli_version_ref": uid("file:CLI_VERSION"),
        "cli_version_build": uid("build:CLI_VERSION"),
    }

    # --- groups ---------------------------------------------------------
    # One group per directory, mirroring the tree so the navigator matches
    # the repository rather than a flat list.
    dirs: dict[str, list[pathlib.Path]] = {}
    for path in app_sources + test_sources:
        dirs.setdefault(str(path.parent), []).append(path)

    lines: list[str] = []
    add = lines.append

    add("// !$*UTF8*$!")
    add("{")
    add("\tarchiveVersion = 1;")
    add("\tclasses = {")
    add("\t};")
    add("\tobjectVersion = 56;")
    add(f"\tobjects = {{")

    # --- PBXBuildFile ---------------------------------------------------
    add("\n/* Begin PBXBuildFile section */")
    for path in app_sources:
        add(f"\t\t{uid('build:' + str(path))} /* {path.name} in Sources */ = "
            f"{{isa = PBXBuildFile; fileRef = {uid('file:' + str(path))} "
            f"/* {path.name} */; }};")
    for path in test_sources:
        add(f"\t\t{uid('build:' + str(path))} /* {path.name} in Sources */ = "
            f"{{isa = PBXBuildFile; fileRef = {uid('file:' + str(path))} "
            f"/* {path.name} */; }};")
    # CLI_VERSION belongs to the REPOSITORY, not to this app, and is carried
    # into the test bundle so the suite can assert REQUIRES_ARCHIVE_LAYOUT
    # against the app's own constant.
    add(f"\t\t{ids['cli_version_build']} /* CLI_VERSION in Resources */ = "
        f"{{isa = PBXBuildFile; fileRef = {ids['cli_version_ref']} "
        f"/* CLI_VERSION */; }};")
    add("/* End PBXBuildFile section */")

    # --- PBXContainerItemProxy ------------------------------------------
    add("\n/* Begin PBXContainerItemProxy section */")
    add(f"\t\t{ids['container_proxy']} /* PBXContainerItemProxy */ = {{")
    add("\t\t\tisa = PBXContainerItemProxy;")
    add(f"\t\t\tcontainerPortal = {ids['project']} /* Project object */;")
    add("\t\t\tproxyType = 1;")
    add(f"\t\t\tremoteGlobalIDString = {ids['app_target']};")
    add(f"\t\t\tremoteInfo = {quote(APP_TARGET)};")
    add("\t\t};")
    add("/* End PBXContainerItemProxy section */")

    # --- PBXFileReference -----------------------------------------------
    add("\n/* Begin PBXFileReference section */")
    for path in app_sources + test_sources:
        add(f"\t\t{uid('file:' + str(path))} /* {path.name} */ = "
            f"{{isa = PBXFileReference; lastKnownFileType = sourcecode.swift; "
            f"path = {quote(path.name)}; sourceTree = \"<group>\"; }};")
    add(f"\t\t{ids['cli_version_ref']} /* CLI_VERSION */ = "
        f"{{isa = PBXFileReference; lastKnownFileType = text; name = CLI_VERSION; "
        f"path = ../CLI_VERSION; sourceTree = \"<group>\"; }};")
    add(f"\t\t{ids['app_product']} /* {APP_TARGET}.app */ = "
        f"{{isa = PBXFileReference; explicitFileType = wrapper.application; "
        f"includeInIndex = 0; path = {quote(APP_TARGET + '.app')}; "
        f"sourceTree = BUILT_PRODUCTS_DIR; }};")
    add(f"\t\t{ids['test_product']} /* {TEST_TARGET}.xctest */ = "
        f"{{isa = PBXFileReference; explicitFileType = wrapper.cfbundle; "
        f"includeInIndex = 0; path = {quote(TEST_TARGET + '.xctest')}; "
        f"sourceTree = BUILT_PRODUCTS_DIR; }};")
    add("/* End PBXFileReference section */")

    # --- PBXFrameworksBuildPhase ----------------------------------------
    # Empty on purpose: every framework this app uses is auto-linked from its
    # import statement, and there is no third-party dependency to link.
    add("\n/* Begin PBXFrameworksBuildPhase section */")
    for phase in (ids["app_frameworks_phase"], ids["test_frameworks_phase"]):
        add(f"\t\t{phase} /* Frameworks */ = {{")
        add("\t\t\tisa = PBXFrameworksBuildPhase;")
        add("\t\t\tbuildActionMask = 2147483647;")
        add("\t\t\tfiles = (")
        add("\t\t\t);")
        add("\t\t\trunOnlyForDeploymentPostprocessing = 0;")
        add("\t\t};")
    add("/* End PBXFrameworksBuildPhase section */")

    # --- PBXGroup --------------------------------------------------------
    add("\n/* Begin PBXGroup section */")

    def group_children(dir_key: str) -> list[str]:
        """Subgroups first, then files, both sorted -- the navigator's order."""
        out = []
        prefix = "" if dir_key == "." else dir_key + "/"
        subdirs = sorted({
            d for d in dirs
            if d != dir_key and d.startswith(prefix) and "/" not in d[len(prefix):]
        })
        for sub in subdirs:
            out.append(f"\t\t\t\t{uid('group:' + sub)} /* {pathlib.Path(sub).name} */,")
        for path in sorted(dirs.get(dir_key, [])):
            out.append(f"\t\t\t\t{uid('file:' + str(path))} /* {path.name} */,")
        return out

    all_group_dirs = sorted({d for d in dirs} | {"Sources", "Tests"})
    for dir_key in all_group_dirs:
        name = pathlib.Path(dir_key).name
        add(f"\t\t{uid('group:' + dir_key)} /* {name} */ = {{")
        add("\t\t\tisa = PBXGroup;")
        add("\t\t\tchildren = (")
        for child in group_children(dir_key):
            add(child)
        add("\t\t\t);")
        add(f"\t\t\tpath = {quote(name)};")
        add("\t\t\tsourceTree = \"<group>\";")
        add("\t\t};")

    add(f"\t\t{ids['main_group']} = {{")
    add("\t\t\tisa = PBXGroup;")
    add("\t\t\tchildren = (")
    add(f"\t\t\t\t{uid('group:Sources')} /* Sources */,")
    add(f"\t\t\t\t{uid('group:Tests')} /* Tests */,")
    add(f"\t\t\t\t{ids['cli_version_ref']} /* CLI_VERSION */,")
    add(f"\t\t\t\t{ids['products_group']} /* Products */,")
    add("\t\t\t);")
    add("\t\t\tsourceTree = \"<group>\";")
    add("\t\t};")

    add(f"\t\t{ids['products_group']} /* Products */ = {{")
    add("\t\t\tisa = PBXGroup;")
    add("\t\t\tchildren = (")
    add(f"\t\t\t\t{ids['app_product']} /* {APP_TARGET}.app */,")
    add(f"\t\t\t\t{ids['test_product']} /* {TEST_TARGET}.xctest */,")
    add("\t\t\t);")
    add("\t\t\tname = Products;")
    add("\t\t\tsourceTree = \"<group>\";")
    add("\t\t};")
    add("/* End PBXGroup section */")

    # --- PBXNativeTarget --------------------------------------------------
    add("\n/* Begin PBXNativeTarget section */")
    add(f"\t\t{ids['app_target']} /* {APP_TARGET} */ = {{")
    add("\t\t\tisa = PBXNativeTarget;")
    add(f"\t\t\tbuildConfigurationList = {ids['app_config_list']};")
    add("\t\t\tbuildPhases = (")
    add(f"\t\t\t\t{ids['app_sources_phase']} /* Sources */,")
    add(f"\t\t\t\t{ids['app_frameworks_phase']} /* Frameworks */,")
    add(f"\t\t\t\t{ids['app_resources_phase']} /* Resources */,")
    add("\t\t\t);")
    add("\t\t\tbuildRules = (")
    add("\t\t\t);")
    add("\t\t\tdependencies = (")
    add("\t\t\t);")
    add(f"\t\t\tname = {quote(APP_TARGET)};")
    add(f"\t\t\tproductName = {quote(APP_TARGET)};")
    add(f"\t\t\tproductReference = {ids['app_product']};")
    add("\t\t\tproductType = \"com.apple.product-type.application\";")
    add("\t\t};")

    add(f"\t\t{ids['test_target']} /* {TEST_TARGET} */ = {{")
    add("\t\t\tisa = PBXNativeTarget;")
    add(f"\t\t\tbuildConfigurationList = {ids['test_config_list']};")
    add("\t\t\tbuildPhases = (")
    add(f"\t\t\t\t{ids['test_sources_phase']} /* Sources */,")
    add(f"\t\t\t\t{ids['test_frameworks_phase']} /* Frameworks */,")
    add(f"\t\t\t\t{ids['test_resources_phase']} /* Resources */,")
    add("\t\t\t);")
    add("\t\t\tbuildRules = (")
    add("\t\t\t);")
    add("\t\t\tdependencies = (")
    add(f"\t\t\t\t{ids['dependency']} /* PBXTargetDependency */,")
    add("\t\t\t);")
    add(f"\t\t\tname = {quote(TEST_TARGET)};")
    add(f"\t\t\tproductName = {quote(TEST_TARGET)};")
    add(f"\t\t\tproductReference = {ids['test_product']};")
    add("\t\t\tproductType = \"com.apple.product-type.bundle.unit-test\";")
    add("\t\t};")
    add("/* End PBXNativeTarget section */")

    # --- PBXProject -------------------------------------------------------
    add("\n/* Begin PBXProject section */")
    add(f"\t\t{ids['project']} /* Project object */ = {{")
    add("\t\t\tisa = PBXProject;")
    add("\t\t\tattributes = {")
    add("\t\t\t\tBuildIndependentTargetsInParallel = 1;")
    add("\t\t\t\tLastSwiftUpdateCheck = 1500;")
    add("\t\t\t\tLastUpgradeCheck = 1500;")
    add("\t\t\t\tTargetAttributes = {")
    add(f"\t\t\t\t\t{ids['app_target']} = {{")
    add("\t\t\t\t\t\tCreatedOnToolsVersion = 15.0;")
    add("\t\t\t\t\t};")
    add(f"\t\t\t\t\t{ids['test_target']} = {{")
    add("\t\t\t\t\t\tCreatedOnToolsVersion = 15.0;")
    add(f"\t\t\t\t\t\tTestTargetID = {ids['app_target']};")
    add("\t\t\t\t\t};")
    add("\t\t\t\t};")
    add("\t\t\t};")
    add(f"\t\t\tbuildConfigurationList = {ids['project_config_list']};")
    add("\t\t\tcompatibilityVersion = \"Xcode 14.0\";")
    add("\t\t\tdevelopmentRegion = en;")
    add("\t\t\thasScannedForEncodings = 0;")
    add("\t\t\tknownRegions = (")
    add("\t\t\t\ten,")
    add("\t\t\t\tBase,")
    add("\t\t\t);")
    add(f"\t\t\tmainGroup = {ids['main_group']};")
    add(f"\t\t\tproductRefGroup = {ids['products_group']} /* Products */;")
    add("\t\t\tprojectDirPath = \"\";")
    add("\t\t\tprojectRoot = \"\";")
    add("\t\t\ttargets = (")
    add(f"\t\t\t\t{ids['app_target']} /* {APP_TARGET} */,")
    add(f"\t\t\t\t{ids['test_target']} /* {TEST_TARGET} */,")
    add("\t\t\t);")
    add("\t\t};")
    add("/* End PBXProject section */")

    # --- PBXResourcesBuildPhase ------------------------------------------
    add("\n/* Begin PBXResourcesBuildPhase section */")
    add(f"\t\t{ids['app_resources_phase']} /* Resources */ = {{")
    add("\t\t\tisa = PBXResourcesBuildPhase;")
    add("\t\t\tbuildActionMask = 2147483647;")
    add("\t\t\tfiles = (")
    add("\t\t\t);")
    add("\t\t\trunOnlyForDeploymentPostprocessing = 0;")
    add("\t\t};")
    add(f"\t\t{ids['test_resources_phase']} /* Resources */ = {{")
    add("\t\t\tisa = PBXResourcesBuildPhase;")
    add("\t\t\tbuildActionMask = 2147483647;")
    add("\t\t\tfiles = (")
    add(f"\t\t\t\t{ids['cli_version_build']} /* CLI_VERSION in Resources */,")
    add("\t\t\t);")
    add("\t\t\trunOnlyForDeploymentPostprocessing = 0;")
    add("\t\t};")
    add("/* End PBXResourcesBuildPhase section */")

    # --- PBXSourcesBuildPhase --------------------------------------------
    add("\n/* Begin PBXSourcesBuildPhase section */")
    for phase, files in (
        (ids["app_sources_phase"], app_sources),
        (ids["test_sources_phase"], test_sources),
    ):
        add(f"\t\t{phase} /* Sources */ = {{")
        add("\t\t\tisa = PBXSourcesBuildPhase;")
        add("\t\t\tbuildActionMask = 2147483647;")
        add("\t\t\tfiles = (")
        for path in files:
            add(f"\t\t\t\t{uid('build:' + str(path))} /* {path.name} in Sources */,")
        add("\t\t\t);")
        add("\t\t\trunOnlyForDeploymentPostprocessing = 0;")
        add("\t\t};")
    add("/* End PBXSourcesBuildPhase section */")

    # --- PBXTargetDependency ---------------------------------------------
    add("\n/* Begin PBXTargetDependency section */")
    add(f"\t\t{ids['dependency']} /* PBXTargetDependency */ = {{")
    add("\t\t\tisa = PBXTargetDependency;")
    add(f"\t\t\ttarget = {ids['app_target']} /* {APP_TARGET} */;")
    add(f"\t\t\ttargetProxy = {ids['container_proxy']} /* PBXContainerItemProxy */;")
    add("\t\t};")
    add("/* End PBXTargetDependency section */")

    # --- XCBuildConfiguration ---------------------------------------------
    shared_project = {
        "ALWAYS_SEARCH_USER_PATHS": "NO",
        "CLANG_ENABLE_OBJC_ARC": "YES",
        "COPY_PHASE_STRIP": "NO",
        "ENABLE_STRICT_OBJC_MSGSEND": "YES",
        "GCC_NO_COMMON_BLOCKS": "YES",
        "MACOSX_DEPLOYMENT_TARGET": DEPLOYMENT_TARGET,
        "SDKROOT": "macosx",
        "SWIFT_VERSION": SWIFT_VERSION,
        # Warnings are NOT errors yet. The Linux app builds with -Werror and
        # this should too -- but nothing in this app has been through a Swift
        # compiler, so turning it on before the first clean build would mean a
        # deprecation notice failing the build for a reason unrelated to the
        # change being made. Flip this to YES once it compiles clean.
        "SWIFT_TREAT_WARNINGS_AS_ERRORS": "NO",
        "SWIFT_STRICT_CONCURRENCY": "minimal",
    }
    debug_only = {
        "DEBUG_INFORMATION_FORMAT": "dwarf",
        "ENABLE_TESTABILITY": "YES",
        "GCC_OPTIMIZATION_LEVEL": "0",
        "MTL_ENABLE_DEBUG_INFO": "INCLUDE_SOURCE",
        "ONLY_ACTIVE_ARCH": "YES",
        "SWIFT_ACTIVE_COMPILATION_CONDITIONS": "DEBUG",
        "SWIFT_OPTIMIZATION_LEVEL": "\"-Onone\"",
    }
    release_only = {
        "DEBUG_INFORMATION_FORMAT": "\"dwarf-with-dsym\"",
        "ENABLE_NS_ASSERTIONS": "NO",
        "MTL_ENABLE_DEBUG_INFO": "NO",
        "SWIFT_COMPILATION_MODE": "wholemodule",
    }

    app_common = {
        "CODE_SIGN_IDENTITY": "\"-\"",
        "CODE_SIGN_STYLE": "Automatic",
        "COMBINE_HIDPI_IMAGES": "YES",
        "CURRENT_PROJECT_VERSION": "1",
        "DEAD_CODE_STRIPPING": "YES",
        # Unsandboxed, deliberately: this app reads an archive anywhere the user
        # keeps one and spawns the installed pipeline. A sandboxed build would
        # need a security-scoped bookmark for every folder and could not run
        # pwsh at all.
        "ENABLE_APP_SANDBOX": "NO",
        "ENABLE_HARDENED_RUNTIME": "YES",
        "GENERATE_INFOPLIST_FILE": "YES",
        "INFOPLIST_KEY_LSApplicationCategoryType": "\"public.app-category.utilities\"",
        "INFOPLIST_KEY_NSHumanReadableCopyright": "\"\"",
        "MARKETING_VERSION": MARKETING_VERSION,
        "PRODUCT_BUNDLE_IDENTIFIER": BUNDLE_ID,
        "PRODUCT_MODULE_NAME": APP_MODULE,
        "PRODUCT_NAME": "\"$(TARGET_NAME)\"",
        "SWIFT_EMIT_LOC_STRINGS": "YES",
    }
    test_common = {
        "BUNDLE_LOADER": "\"$(TEST_HOST)\"",
        "CODE_SIGN_IDENTITY": "\"-\"",
        "CODE_SIGN_STYLE": "Automatic",
        "CURRENT_PROJECT_VERSION": "1",
        "GENERATE_INFOPLIST_FILE": "YES",
        "MARKETING_VERSION": MARKETING_VERSION,
        "PRODUCT_BUNDLE_IDENTIFIER": BUNDLE_ID + "Tests",
        "PRODUCT_MODULE_NAME": APP_MODULE + "Tests",
        "PRODUCT_NAME": "\"$(TARGET_NAME)\"",
        "TEST_HOST": f"\"$(BUILT_PRODUCTS_DIR)/{APP_TARGET}.app/Contents/MacOS/{APP_TARGET}\"",
    }

    add("\n/* Begin XCBuildConfiguration section */")
    for cfg_id, name, extra in (
        (ids["project_debug"], "Debug", {**shared_project, **debug_only}),
        (ids["project_release"], "Release", {**shared_project, **release_only}),
        (ids["app_debug"], "Debug", app_common),
        (ids["app_release"], "Release", app_common),
        (ids["test_debug"], "Debug", test_common),
        (ids["test_release"], "Release", test_common),
    ):
        add(f"\t\t{cfg_id} /* {name} */ = {{")
        add("\t\t\tisa = XCBuildConfiguration;")
        add("\t\t\tbuildSettings = {")
        add(settings_block(extra, "\t\t\t\t"))
        add("\t\t\t};")
        add(f"\t\t\tname = {name};")
        add("\t\t};")
    add("/* End XCBuildConfiguration section */")

    # --- XCConfigurationList ----------------------------------------------
    add("\n/* Begin XCConfigurationList section */")
    for list_id, label, debug_id, release_id in (
        (ids["project_config_list"], "PBXProject", ids["project_debug"], ids["project_release"]),
        (ids["app_config_list"], APP_TARGET, ids["app_debug"], ids["app_release"]),
        (ids["test_config_list"], TEST_TARGET, ids["test_debug"], ids["test_release"]),
    ):
        add(f"\t\t{list_id} /* Build configuration list for {label} */ = {{")
        add("\t\t\tisa = XCConfigurationList;")
        add("\t\t\tbuildConfigurations = (")
        add(f"\t\t\t\t{debug_id} /* Debug */,")
        add(f"\t\t\t\t{release_id} /* Release */,")
        add("\t\t\t);")
        add("\t\t\tdefaultConfigurationIsVisible = 0;")
        add("\t\t\tdefaultConfigurationName = Release;")
        add("\t\t};")
    add("/* End XCConfigurationList section */")

    add("\t};")
    add(f"\trootObject = {ids['project']} /* Project object */;")
    add("}")

    proj_dir = ROOT / f"{PROJECT_NAME}.xcodeproj"
    (proj_dir / "xcshareddata" / "xcschemes").mkdir(parents=True, exist_ok=True)
    (proj_dir / "project.xcworkspace").mkdir(parents=True, exist_ok=True)

    (proj_dir / "project.pbxproj").write_text("\n".join(lines) + "\n")

    (proj_dir / "project.xcworkspace" / "contents.xcworkspacedata").write_text(
        '<?xml version="1.0" encoding="UTF-8"?>\n'
        '<Workspace\n'
        '   version = "1.0">\n'
        '   <FileRef\n'
        '      location = "self:">\n'
        '   </FileRef>\n'
        '</Workspace>\n'
    )

    # A SHARED scheme, so `xcodebuild -scheme ytdl-macos test` works from a
    # clean checkout. Xcode invents one on first open; a checkout that has never
    # been opened in Xcode has nothing to build.
    scheme = f"""<?xml version="1.0" encoding="UTF-8"?>
<Scheme
   LastUpgradeVersion = "1500"
   version = "1.7">
   <BuildAction
      parallelizeBuildables = "YES"
      buildImplicitDependencies = "YES">
      <BuildActionEntries>
         <BuildActionEntry
            buildForTesting = "YES"
            buildForRunning = "YES"
            buildForProfiling = "YES"
            buildForArchiving = "YES"
            buildForAnalyzing = "YES">
            <BuildableReference
               BuildableIdentifier = "primary"
               BlueprintIdentifier = "{ids['app_target']}"
               BuildableName = "{APP_TARGET}.app"
               BlueprintName = "{APP_TARGET}"
               ReferencedContainer = "container:{PROJECT_NAME}.xcodeproj">
            </BuildableReference>
         </BuildActionEntry>
      </BuildActionEntries>
   </BuildAction>
   <TestAction
      buildConfiguration = "Debug"
      selectedDebuggerIdentifier = "Xcode.DebuggerFoundation.Debugger.LLDB"
      selectedLauncherIdentifier = "Xcode.DebuggerFoundation.Launcher.LLDB"
      shouldUseLaunchSchemeArgsEnv = "YES">
      <Testables>
         <TestableReference
            skipped = "NO">
            <BuildableReference
               BuildableIdentifier = "primary"
               BlueprintIdentifier = "{ids['test_target']}"
               BuildableName = "{TEST_TARGET}.xctest"
               BlueprintName = "{TEST_TARGET}"
               ReferencedContainer = "container:{PROJECT_NAME}.xcodeproj">
            </BuildableReference>
         </TestableReference>
      </Testables>
   </TestAction>
   <LaunchAction
      buildConfiguration = "Debug"
      selectedDebuggerIdentifier = "Xcode.DebuggerFoundation.Debugger.LLDB"
      selectedLauncherIdentifier = "Xcode.DebuggerFoundation.Launcher.LLDB"
      launchStyle = "0"
      useCustomWorkingDirectory = "NO"
      ignoresPersistentStateOnLaunch = "NO"
      debugDocumentVersioning = "YES"
      debugServiceExtension = "internal"
      allowLocationSimulation = "YES">
      <BuildableProductRunnable
         runnableDebuggingMode = "0">
         <BuildableReference
            BuildableIdentifier = "primary"
            BlueprintIdentifier = "{ids['app_target']}"
            BuildableName = "{APP_TARGET}.app"
            BlueprintName = "{APP_TARGET}"
            ReferencedContainer = "container:{PROJECT_NAME}.xcodeproj">
         </BuildableReference>
      </BuildableProductRunnable>
   </LaunchAction>
   <ProfileAction
      buildConfiguration = "Release"
      shouldUseLaunchSchemeArgsEnv = "YES"
      savedToolIdentifier = ""
      useCustomWorkingDirectory = "NO"
      debugDocumentVersioning = "YES">
      <BuildableProductRunnable
         runnableDebuggingMode = "0">
         <BuildableReference
            BuildableIdentifier = "primary"
            BlueprintIdentifier = "{ids['app_target']}"
            BuildableName = "{APP_TARGET}.app"
            BlueprintName = "{APP_TARGET}"
            ReferencedContainer = "container:{PROJECT_NAME}.xcodeproj">
         </BuildableReference>
      </BuildableProductRunnable>
   </ProfileAction>
   <AnalyzeAction
      buildConfiguration = "Debug">
   </AnalyzeAction>
   <ArchiveAction
      buildConfiguration = "Release"
      revealArchiveInOrganizer = "YES">
   </ArchiveAction>
</Scheme>
"""
    (proj_dir / "xcshareddata" / "xcschemes" / f"{APP_TARGET}.xcscheme").write_text(scheme)

    print(f"wrote {proj_dir.relative_to(REPO)} "
          f"({len(app_sources)} app sources, {len(test_sources)} test sources)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
