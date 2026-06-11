"""Generative builder that fills LSP params with hostile enum values.

Walks lsprotocol type metadata recursively and builds wire-format (camelCase)
JSON for a params type, injecting unknown string enum values, out-of-range
integer enum values, or unknown object fields depending on the mode.
"""

import enum
import types
import typing

import attrs

UNKNOWN_STRING = "cliceUnknownEnumValue"
OUT_OF_RANGE_INT = 9999
UNKNOWN_FIELD = "cliceUnknownField"

MAX_DEPTH = 14


class InjectionStats:
    def __init__(self) -> None:
        self.string_enums = 0
        self.int_enums = 0
        self.unknown_fields = 0


def _camel(name: str) -> str:
    name = name.rstrip("_")
    head, *rest = name.split("_")
    return head + "".join(part.capitalize() for part in rest)


def _is_union(tp) -> bool:
    return typing.get_origin(tp) in (typing.Union, types.UnionType)


def _union_priority(tp) -> int:
    if isinstance(tp, type) and attrs.has(tp):
        return 0
    if isinstance(tp, type) and issubclass(tp, enum.Enum):
        return 1
    if typing.get_origin(tp) is not None:
        return 2
    return 3


def _enum_is_string(tp) -> bool:
    return isinstance(next(iter(tp)).value, str)


class Builder:
    def __init__(self, mode: str) -> None:
        assert mode in (
            "unknown_string_enums",
            "out_of_range_int_enums",
            "unknown_fields",
        )
        self.mode = mode
        self.stats = InjectionStats()

    def build(self, tp, depth: int = 0):
        if depth > MAX_DEPTH:
            return None

        if tp is type(None):
            return None
        if tp is typing.Any:
            return {"cliceAny": True}
        if isinstance(tp, type) and issubclass(tp, enum.Enum):
            return self._build_enum(tp)
        if isinstance(tp, type) and attrs.has(tp):
            return self._build_object(tp, depth)

        origin = typing.get_origin(tp)
        args = typing.get_args(tp)

        if _is_union(tp):
            candidates = [a for a in args if a is not type(None)]
            candidates.sort(key=_union_priority)
            return self.build(candidates[0], depth) if candidates else None
        if origin is typing.Literal:
            return args[0]
        if origin in (list, tuple, typing.Sequence) or (
            origin is not None and origin.__name__ in ("Sequence", "List")
        ):
            element = args[0] if args else typing.Any
            return self._build_sequence(element, depth)
        if origin in (dict,) or (
            origin is not None and origin.__name__ in ("Mapping", "Dict")
        ):
            return {
                "key": self.build(args[1] if len(args) > 1 else typing.Any, depth + 1)
            }

        if tp is bool:
            return True
        if tp is int:
            return 1
        if tp is float:
            return 1.0
        if tp is str:
            return "text"
        return None

    def _build_enum(self, tp):
        valid = next(iter(tp)).value
        if _enum_is_string(tp):
            if self.mode == "unknown_string_enums":
                self.stats.string_enums += 1
                return UNKNOWN_STRING
            return valid
        if self.mode == "out_of_range_int_enums":
            self.stats.int_enums += 1
            return OUT_OF_RANGE_INT
        return valid

    def _build_sequence(self, element, depth: int):
        items = [self.build(element, depth + 1)]
        # Enum arrays (valueSets) get a valid member plus the injected one,
        # so known and unknown values must coexist in one array.
        if isinstance(element, type) and issubclass(element, enum.Enum):
            items.append(next(iter(element)).value)
        return [item for item in items if item is not None]

    def _build_object(self, tp, depth: int):
        hints = typing.get_type_hints(tp)
        out = {}
        for field in attrs.fields(tp):
            value = self.build(hints[field.name], depth + 1)
            if value is not None:
                out[_camel(field.name)] = value
        if self.mode == "unknown_fields":
            self.stats.unknown_fields += 1
            out[UNKNOWN_FIELD] = {"cliceNested": [1, "two", None]}
        return out


def build_params(tp, mode: str) -> tuple[dict, InjectionStats]:
    builder = Builder(mode)
    params = builder.build(tp)
    return params, builder.stats
