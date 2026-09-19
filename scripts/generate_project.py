#!/usr/bin/env python3
"""Deterministic, dependency-free Xcode project for the implemented app targets."""
import hashlib
import json
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

def app_customization():
    values = {"app_name": "AndroidEmu", "bundle_id": "org.androidemu.app"}
    path = ROOT / "build/app-customization.json"
    if path.is_file():
        loaded = json.loads(path.read_text(encoding="utf-8"))
        if isinstance(loaded.get("app_name"), str) and loaded["app_name"]:
            values["app_name"] = loaded["app_name"]
        if isinstance(loaded.get("bundle_id"), str) and loaded["bundle_id"]:
            values["bundle_id"] = loaded["bundle_id"]
    return values

def ident(name):
    return hashlib.sha256(name.encode()).hexdigest()[:24].upper()

def quote(text):
    return '"' + text.replace('\\', '\\\\').replace('"', '\\"') + '"'

def generate():
    customization = app_customization()
    files = []
    for folder in ('App', 'Core', 'ImageKit', 'JIT', 'QEMUBridge', 'Display', 'Input', 'Audio', 'ADB', 'Network', 'Performance', 'ThirdParty/StikJITProtocol'):
        files.extend(p.relative_to(ROOT).as_posix() for p in (ROOT / folder).rglob('*') if p.suffix in ('.swift', '.cpp', '.mm', '.c', '.metal'))
    files.sort()
    objects = []
    def obj(name, body):
        objects.append(f'\t\t{ident(name)} = {{ {body} }};')
    source_refs, build_refs = [], []
    kinds = {'.swift': 'sourcecode.swift', '.cpp': 'sourcecode.cpp.cpp', '.mm': 'sourcecode.cpp.objcpp', '.c': 'sourcecode.c.c', '.metal': 'sourcecode.metal'}
    for file in files:
        source_refs.append(ident(file)); build_refs.append(ident('build:' + file))
        obj(file, f'isa = PBXFileReference; lastKnownFileType = {kinds[Path(file).suffix]}; path = {quote(file)}; sourceTree = SOURCE_ROOT;')
        obj('build:' + file, f'isa = PBXBuildFile; fileRef = {ident(file)};')
    frameworks = ['UIKit', 'SwiftUI', 'Metal', 'MetalKit', 'AVFoundation', 'Security', 'CoreFoundation', 'Network', 'QuartzCore']
    framework_refs, framework_builds = [], []
    for name in frameworks:
        framework_refs.append(ident(name)); framework_builds.append(ident('link:' + name))
        obj(name, f'isa = PBXFileReference; lastKnownFileType = wrapper.framework; path = System/Library/Frameworks/{name}.framework; sourceTree = SDKROOT;')
        obj('link:' + name, f'isa = PBXBuildFile; fileRef = {ident(name)};')
    resource_refs, resource_builds = [], []
    resources = ['LICENSE', 'THIRD_PARTY_NOTICES.md', 'ThirdParty/StikJITProtocol/MPL-2.0.txt',
                 'build/guest-kernel/goldfish-highmem.zImage', 'build/guest-kernel/goldfish-kernel-COPYING.txt']
    icon_catalog = ROOT / 'build/AppAssets.xcassets'
    if icon_catalog.is_dir():
        resources.append('build/AppAssets.xcassets')
    for resource in resources:
        resource_refs.append(ident(resource)); resource_builds.append(ident('resource:' + resource))
        file_type = 'folder.assetcatalog' if resource.endswith('.xcassets') else 'text'
        obj(resource, f'isa = PBXFileReference; lastKnownFileType = {file_type}; path = {quote(resource)}; sourceTree = SOURCE_ROOT;')
        obj('resource:' + resource, f'isa = PBXBuildFile; fileRef = {ident(resource)};')
    obj('product', 'isa = PBXFileReference; explicitFileType = wrapper.application; path = AndroidEmu.app; sourceTree = BUILT_PRODUCTS_DIR;')
    obj('products', f'isa = PBXGroup; children = ({ident("product")},); name = Products; sourceTree = "<group>";')
    obj('rootgroup', f'isa = PBXGroup; children = ({",".join(source_refs + resource_refs + framework_refs + [ident("products")])},); sourceTree = "<group>";')
    obj('sources', f'isa = PBXSourcesBuildPhase; buildActionMask = 2147483647; files = ({",".join(build_refs)},); runOnlyForDeploymentPostprocessing = 0;')
    obj('frameworks', f'isa = PBXFrameworksBuildPhase; buildActionMask = 2147483647; files = ({",".join(framework_builds)},); runOnlyForDeploymentPostprocessing = 0;')
    obj('resources', f'isa = PBXResourcesBuildPhase; buildActionMask = 2147483647; files = ({",".join(resource_builds)},); runOnlyForDeploymentPostprocessing = 0;')
    common = {
        'OTHER_LDFLAGS': '$(inherited) -lz',
        'SDKROOT': 'iphoneos', 'IPHONEOS_DEPLOYMENT_TARGET': '17.0', 'ARCHS': 'arm64',
        'SUPPORTED_PLATFORMS': 'iphoneos', 'SUPPORTS_MACCATALYST': 'NO', 'TARGETED_DEVICE_FAMILY': '1,2',
        'SWIFT_VERSION': '5.0', 'CLANG_CXX_LANGUAGE_STANDARD': 'c++20', 'CLANG_CXX_LIBRARY': 'libc++',
        'CLANG_ENABLE_OBJC_ARC': 'YES', 'GCC_WARN_INHIBIT_ALL_WARNINGS': 'NO',
        'GCC_TREAT_WARNINGS_AS_ERRORS': 'YES', 'SWIFT_TREAT_WARNINGS_AS_ERRORS': 'YES',
        'CLANG_WARN_DOCUMENTATION_COMMENTS': 'YES', 'CLANG_WARN_OBJC_IMPLICIT_RETAIN_SELF': 'YES',
        'ENABLE_USER_SCRIPT_SANDBOXING': 'YES', 'CODE_SIGNING_ALLOWED': 'NO', 'CODE_SIGNING_REQUIRED': 'NO',
        'PRODUCT_BUNDLE_IDENTIFIER': customization['bundle_id'], 'PRODUCT_NAME': 'AndroidEmu',
        'APP_DISPLAY_NAME': customization['app_name'],
        'INFOPLIST_FILE': 'App/Info.plist', 'GENERATE_INFOPLIST_FILE': 'NO',
        'SWIFT_OBJC_BRIDGING_HEADER': 'QEMUBridge/NativeBridge.h', 'HEADER_SEARCH_PATHS': '$(SRCROOT)',
        'LD_RUNPATH_SEARCH_PATHS': '@executable_path/Frameworks',
        'CODE_SIGN_ENTITLEMENTS': 'AndroidEmu.entitlements', 'ENABLE_BITCODE': 'NO',
    }
    if icon_catalog.is_dir():
        common['ASSETCATALOG_COMPILER_APPICON_NAME'] = 'AppIcon'
    for mode in ('Debug', 'Release'):
        settings = dict(common)
        settings.update({'SWIFT_OPTIMIZATION_LEVEL': '-Onone' if mode == 'Debug' else '-O',
                         'GCC_OPTIMIZATION_LEVEL': '0' if mode == 'Debug' else '3',
                         'DEBUG_INFORMATION_FORMAT': 'dwarf' if mode == 'Debug' else 'dwarf-with-dsym'})
        body = ' '.join(f'{key} = {quote(value)};' for key, value in sorted(settings.items()))
        obj('target:' + mode, f'isa = XCBuildConfiguration; buildSettings = {{ {body} }}; name = {mode};')
        obj('project:' + mode, f'isa = XCBuildConfiguration; buildSettings = {{}}; name = {mode};')
    for kind in ('target', 'project'):
        obj(kind + ':configs', f'isa = XCConfigurationList; buildConfigurations = ({ident(kind+":Debug")},{ident(kind+":Release")},); defaultConfigurationIsVisible = 0; defaultConfigurationName = Release;')
    obj('target', f'isa = PBXNativeTarget; buildConfigurationList = {ident("target:configs")}; buildPhases = ({ident("sources")},{ident("frameworks")},{ident("resources")},); buildRules = (); dependencies = (); name = AndroidEmu; productName = AndroidEmu; productReference = {ident("product")}; productType = "com.apple.product-type.application";')
    obj('project', f'isa = PBXProject; attributes = {{ LastUpgradeCheck = 2600; }}; buildConfigurationList = {ident("project:configs")}; compatibilityVersion = "Xcode 14.0"; developmentRegion = en; hasScannedForEncodings = 0; knownRegions = (en,ja,Base,); mainGroup = {ident("rootgroup")}; productRefGroup = {ident("products")}; projectDirPath = ""; projectRoot = ""; targets = ({ident("target")},);')
    project = ROOT / 'AndroidEmu.xcodeproj'
    project.mkdir(exist_ok=True)
    (project / 'project.pbxproj').write_text('// !$*UTF8*$!\n{\n\tarchiveVersion = 1;\n\tclasses = {};\n\tobjectVersion = 56;\n\tobjects = {\n' + '\n'.join(objects) + f'\n\t}};\n\trootObject = {ident("project")};\n}}\n')
    schemes = project / 'xcshareddata/xcschemes'; schemes.mkdir(parents=True, exist_ok=True)
    reference = f'<BuildableReference BuildableIdentifier="primary" BlueprintIdentifier="{ident("target")}" BuildableName="AndroidEmu.app" BlueprintName="AndroidEmu" ReferencedContainer="container:AndroidEmu.xcodeproj"/>'
    (schemes / 'AndroidEmu.xcscheme').write_text(f'''<?xml version="1.0" encoding="UTF-8"?>
<Scheme LastUpgradeVersion="2600" version="1.3">
 <BuildAction parallelizeBuildables="YES" buildImplicitDependencies="YES"><BuildActionEntries><BuildActionEntry buildForTesting="YES" buildForRunning="YES" buildForProfiling="YES" buildForArchiving="YES" buildForAnalyzing="YES">{reference}</BuildActionEntry></BuildActionEntries></BuildAction>
 <LaunchAction buildConfiguration="Debug" selectedDebuggerIdentifier="Xcode.DebuggerFoundation.Debugger.LLDB" selectedLauncherIdentifier="Xcode.IDEFoundation.Launcher.LLDB" launchStyle="0" useCustomWorkingDirectory="NO" ignoresPersistentStateOnLaunch="NO" debugDocumentVersioning="YES" debugServiceExtension="internal" allowLocationSimulation="NO"><BuildableProductRunnable runnableDebuggingMode="0">{reference}</BuildableProductRunnable></LaunchAction>
 <ProfileAction buildConfiguration="Release" shouldUseLaunchSchemeArgsEnv="YES" savedToolIdentifier="" useCustomWorkingDirectory="NO" debugDocumentVersioning="YES"><BuildableProductRunnable runnableDebuggingMode="0">{reference}</BuildableProductRunnable></ProfileAction>
 <AnalyzeAction buildConfiguration="Debug"/>
 <ArchiveAction buildConfiguration="Release" revealArchiveInOrganizer="YES"/>
</Scheme>
''')

if __name__ == '__main__':
    generate()
