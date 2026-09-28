#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Derive an offline maintenance4 measurement selection from pinned CTS sources."""
import argparse
import json
from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
from tools.make_measurement_manifest import build_measurement_manifest

MODULE = 'external/vulkancts/modules/vulkan/spirv_assembly/'
LOCAL_SOURCE = MODULE+'vktSpvAsmInstructionTests.cpp'
MULTIPLE_SOURCE = MODULE+'vktSpvAsmMultipleShadersTests.cpp'
PREFIX = 'dEQP-VK.spirv_assembly.instruction.compute.'


def local_size_paths(text):
    """Fail closed if the bounded factory's loop or naming contract changes."""
    start = text.find('tcu::TestCaseGroup *createLocalSizeGroup(')
    end = text.find('tcu::TestCaseGroup *createOpNopGroup(', start)
    if start < 0 or end < 0:
        return set()
    group = text[start:end]
    compact = re.sub(r'\s+', '', group)
    required = ('for(inti=0;i<DE_LENGTH_OF_ARRAY(cases);i++)',
                'for(intj=0;j<3;j++)', 'for(intk=0;k<3;k++)',
                'newtcu::TestCaseGroup(testCtx,groupName[useLocalSizeId])',
                'LocalSizeValueTypeexecModeType=(LocalSizeValueType)j;',
                'LocalSizeValueTypewgSizeType=(LocalSizeValueType)k;',
                'if(execModeType==LSV_NONE&&wgSizeType==LSV_NONE)continue;',
                'if(execModeType==LSV_SPEC_CONST&&!useLocalSizeId)continue;',
                'spec.extensions.push_back("VK_KHR_maintenance4");',
                'localSizeModeToString(execModeType)+"_wgsize_"+localSizeModeToString(wgSizeType)+cases[i].nameSuffix',
                'getAsmForLocalSizeTest(useLocalSizeId,execModeType,wgSizeType,cases[i].localSize,cases[i].ndx)',
                'group->addChild(newSpvAsmComputeShaderCase(testCtx,testName.c_str(),spec));')
    if any(token not in compact for token in required):
        return set()
    enum = re.search(r'enum LocalSizeValueType\s*\{([^}]+)\}', text)
    if not enum or re.findall(r'LSV_\w+', enum[1]) != ['LSV_NONE','LSV_LITERAL','LSV_SPEC_CONST']:
        return set()
    names = re.search(r'groupName\[\]\s*\{\s*"([^"]+)"\s*,\s*"([^"]+)"\s*\}', group)
    naming_start = text.find('static string localSizeModeToString(')
    naming = text[naming_start:start] if naming_start >= 0 else ''
    labels = dict(re.findall(r'case\s+(LSV_\w+)\s*:\s*return\s*"([^"]+)"', naming))
    if not names or set(labels) != {'LSV_NONE','LSV_LITERAL','LSV_SPEC_CONST'}:
        return set()
    suffixes = re.findall(r'\{"([^"]*)",\s*IVec3\([^)]*\),\s*IVec3\([^)]*\),\s*\d+u?\}', group)
    if not suffixes or len(set(suffixes)) != len(suffixes) or len(set(labels.values())) != 3:
        return set()
    return {PREFIX+names[2]+'.'+labels[execution]+'_wgsize_'+labels[wg]+suffix
            for suffix in suffixes for execution in labels for wg in labels
            if (execution,wg) != ('LSV_NONE','LSV_NONE')}


def multiple_shader_paths(text):
    compact = re.sub(r'\s+', '', text)
    if ('if(testConfig.type==TestType::TWO_ENTRY_POINTS_EXECUTION_MODE_ID)'
        'context.requireDeviceFunctionality("VK_KHR_maintenance4");') not in compact:
        return set()
    factory = text[text.find('tcu::TestCaseGroup *createMultipleShaderExtendedGroup('):]
    group = re.search(r'mainGroup\(new tcu::TestCaseGroup\(testCtx,\s*"([^"]+)"\)\)', factory)
    leaf = re.search(r'TestConfig testConfig = \{TestType::TWO_ENTRY_POINTS_EXECUTION_MODE_ID\};\s*'
                     r'mainGroup->addChild\(new EntryPointsTestCase\(testCtx,\s*"([^"]+)",\s*testConfig,\s*'
                     r'typename FunctionSupport1<TestConfig>::Args\(checkSupport, testConfig\)\)\);', factory)
    return {PREFIX+group[1]+'.'+leaf[1]} if group and leaf else set()


def entries(upstream):
    result = []
    for source, derive in ((LOCAL_SOURCE, local_size_paths), (MULTIPLE_SOURCE, multiple_shader_paths)):
        paths = derive((upstream/source).read_text())
        if not paths:
            raise ValueError('cannot derive maintenance4 leaves from '+source)
        result.extend(dict(path=path, source=source, category='maintenance4-local-size',
                           expected_status='Pass', features_required=['VK_KHR_maintenance4','maintenance4'])
                      for path in sorted(paths))
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    frozen = ROOT/'cts/upstream/manifest.json'
    manifest = json.loads(frozen.read_text())
    additions = entries(ROOT/'third_party/vk-gl-cts')
    known = {c['path'] for c in manifest['cases']+manifest.get('diagnostics', [])}
    if any(c['path'] in known for c in additions):
        raise ValueError('maintenance4 selection overlaps frozen entries; re-audit derivation')
    manifest['diagnostics'].extend(additions)
    result = build_measurement_manifest(manifest, {'maintenance4-local-size'})
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(result, indent=2)+'\n')
    print(f'{len(additions)} original maintenance4 leaves; {len(result["cases"])} total selected; no frozen edits')


if __name__ == '__main__':
    main()
