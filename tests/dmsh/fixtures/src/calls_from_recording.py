# The recorded calls of nxnative's shader build modules (tests.jsonl, recorded while nxnative's unit tests ran) as plain JSON for nxmac's dmsh_test:
# one line per call, {"f": module.function, "a": [args], "r": result} or "e": the exception's type and message. Tuples become lists, sets sorted lists, dicts objects
# (int keys as strings), bytes a hex string; only the functions nxmac ports
import json, sys, gzip
KEEP = {'dmsh_alpha._colour0','dmsh_alpha.entry','dmsh_alpha.refused','dmsh_entry.drop_point_size','dmsh_entry.fs_uint','dmsh_entry.int_outputs',
'dmsh_fetch._entry_screen','dmsh_fetch._frag_pos','dmsh_fetch._once','dmsh_fetch._proj','dmsh_fetch.fold','dmsh_fetch.screen_varyings','dmsh_fetch.select_guards',
'dmsh_geometry.capture','dmsh_geometry.output_words','dmsh_logic.constants','dmsh_logic.entry','dmsh_logic.refused','dmsh_pull._drop_entries','dmsh_pull.variant',
'dmsh_reflect.c2_array','dmsh_reflect.decode_tex','dmsh_reflect.decode_ubo_img','dmsh_reflect.decode_ubo_tex','dmsh_reflect.drv_reads','dmsh_reflect.in_word',
'dmsh_reflect.reflect','dmsh_reflect.unfillable','dmsh_rewrite.blend_zero','dmsh_rewrite.block_end','dmsh_rewrite.border_emu','dmsh_rewrite.first_store',
'dmsh_rewrite.frag_z_snap','dmsh_rewrite.gather_offsets','dmsh_rewrite.gather_quad','dmsh_rewrite.mul_extended','dmsh_rewrite.shared_store_msl',
'dmsh_rewrite.split_params','dmsh_rewrite.tg_spill','dmsh_side._close','dmsh_side._params','dmsh_side.body','dmsh_side.calling','dmsh_side.entries',
'dmsh_side.entry','dmsh_side.shared_body','dmsh_side.unit','dmsh_vin.variant','dmsh_wide.wide_loads'}
def dec(v):
    if isinstance(v, list): return [dec(x) for x in v]
    if isinstance(v, dict):
        if '__tuple__' in v: return [dec(x) for x in v['__tuple__']]
        if '__set__' in v: return [dec(x) for x in v['__set__']]
        if '__bytes__' in v: return v['__bytes__']
        if '__float__' in v: return float(v['__float__'])
        if '__dict__' in v: return {str(k): dec(x) for k, x in v['__dict__']}
        raise ValueError(v)
    return v
src, dst = sys.argv[1:]
n = 0
with gzip.open(dst, 'wt', encoding='utf-8', compresslevel=9) as out:
    for line in open(src, encoding='utf-8'):
        r = json.loads(line)
        if r['f'] not in KEEP: continue
        o = {'f': r['f'].split('.', 1)[1] if False else r['f'], 'a': dec(r['a'])}
        if dec(r['k']): o['k'] = dec(r['k'])
        if 'e' in r: o['e'] = r['e']
        else: o['r'] = dec(r['r'])
        out.write(json.dumps(o, ensure_ascii=False, sort_keys=False) + '\n'); n += 1
print(n, 'calls')
