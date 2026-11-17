"""Proportional CPU-active timing estimates from distclang v1 process totals.

The queue is outside execution_wall_us. A process-wide ratio cannot locate
individual stalls: it estimates every phase with the same factor, capped at
one (parallel CPU accounting must not increase wall time). Missing/invalid or
unknown metadata leaves the original clock intact. Input traces are immutable.
"""


def trace_timing(data):
    metadata = data.get("distclang")
    result = {"status": "missing", "numerator": 1, "denominator": 1,
              "scale": 1.0, "distclang": metadata}
    if "distclang" not in data:
        return result
    result["status"] = "invalid"
    if not isinstance(metadata, dict):
        return result
    version = metadata.get("version")
    if type(version) is not int or not 0 <= version <= 2**63 - 1:
        return result
    if version != 1:
        result["status"] = "unsupported"
        return result
    values = [metadata.get(k) for k in
              ("execution_wall_us", "cpu_user_us", "cpu_system_us")]
    if any(type(v) is not int or not 0 <= v <= 2**63 - 1 for v in values):
        return result
    wall, user, system = values
    cpu = user + system
    if wall <= 0 or not 0 < cpu <= 2**63 - 1:
        return result
    numerator = min(cpu, wall)
    result.update(status="distclang", numerator=numerator, denominator=wall,
                  scale=numerator / wall)
    return result


def scale_time(value, timing):
    """Round to integer microseconds; identical to the native trace reader."""
    n, d = timing["numerator"], timing["denominator"]
    if n == d:
        return value
    scaled = (abs(value) * n + d // 2) // d
    return -scaled if value < 0 else scaled


def normalized_events(data, timing=None):
    """Map the whole relative clock, including async endpoints and Total events.

    Complete events use mapped endpoints, not independently rounded durations:
    touching/nested intervals must still share their boundaries.
    """
    timing = trace_timing(data) if timing is None else timing
    for event in data.get("traceEvents", []):
        if timing["numerator"] == timing["denominator"]:
            yield event
            continue
        out = dict(event)
        ts = event.get("ts", 0)
        if "ts" in event:
            out["ts"] = scale_time(ts, timing)
        if "dur" in event:
            out["dur"] = scale_time(ts + event["dur"], timing) - scale_time(ts, timing)
        yield out
