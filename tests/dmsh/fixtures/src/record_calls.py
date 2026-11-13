# Records the calls of nxnative's shader build modules (their module-level functions): one JSON line per distinct call, {"f": module.function, "a": args, "k": kwargs, "r": result} or "e": exception.
# On when RECORD_OUT names the file. Values: tuples, sets and bytes are marked ({"__tuple__": [...]}, {"__set__": [...]}, {"__bytes__": hex}); a call with anything else is not recorded.
import builtins, functools, hashlib, json, os, sys, threading, types

OUT = os.environ.get('RECORD_OUT')
TARGETS = {'dmsh_rewrite', 'dmsh_fetch', 'dmsh_reflect', 'dmsh_entry', 'dmsh_wide', 'dmsh_atomic', 'dmsh_side', 'dmsh_alpha', 'dmsh_logic', 'dmsh_vin', 'dmsh_pull', 'dmsh_geometry', 'c1_guard', 'cb_extent', 'dmsh_ampere'}
LIMIT = int(os.environ.get('RECORD_LIMIT', str(4 << 20)))

if OUT:
    _lock, _seen, _depth = threading.Lock(), set(), threading.local()

    class Skip(Exception): pass

    def enc(v):
        if v is None or isinstance(v, (bool, int, str)): return v
        if isinstance(v, float): return {'__float__': repr(v)}
        if isinstance(v, list): return [enc(x) for x in v]
        if isinstance(v, tuple): return {'__tuple__': [enc(x) for x in v]}
        if isinstance(v, (set, frozenset)):
            try: return {'__set__': [enc(x) for x in sorted(v)]}
            except TypeError: raise Skip()
        if isinstance(v, (bytes, bytearray)): return {'__bytes__': bytes(v).hex()}
        if isinstance(v, dict):
            if not all(isinstance(k, (str, int)) for k in v): raise Skip()
            return {'__dict__': [[enc(k), enc(x)] for k, x in v.items()]}
        raise Skip()

    def wrap(mod, name, fn):
        @functools.wraps(fn)
        def w(*a, **k):
            try: ea, ek = enc(list(a)), enc(k)
            except Skip: return fn(*a, **k)
            rec = {'f': f'{mod}.{name}', 'a': ea, 'k': ek}
            try: r = fn(*a, **k)
            except Exception as e:
                rec['e'] = f'{type(e).__name__}: {e}'; emit(rec); raise
            try: rec['r'] = enc(r)
            except Skip: return r
            emit(rec)
            return r
        w.__wrapped_for_record__ = True
        return w

    def emit(rec):
        s = json.dumps(rec, ensure_ascii=False)
        if len(s) > LIMIT: return
        h = hashlib.sha256(s.encode()).digest()
        with _lock:
            if h in _seen: return
            _seen.add(h)
            with open(OUT, 'a', encoding='utf-8') as f: f.write(s + '\n')

    def instrument(m):
        for name, fn in list(vars(m).items()):
            if isinstance(fn, types.FunctionType) and fn.__module__ == m.__name__ and not getattr(fn, '__wrapped_for_record__', False) and not name.startswith('__'):
                if fn.__code__.co_flags & 0x20: continue   # a generator
                setattr(m, name, wrap(m.__name__, name, fn))

    _import = builtins.__import__
    def hook(name, globals=None, locals=None, fromlist=(), level=0):
        m = _import(name, globals, locals, fromlist, level)
        for t in TARGETS:
            mod = sys.modules.get(t)
            if mod is not None and not getattr(mod, '__recorded__', False) and getattr(getattr(mod, '__spec__', None), '_initializing', False) is False:
                mod.__recorded__ = True; instrument(mod)
        return m
    builtins.__import__ = hook
