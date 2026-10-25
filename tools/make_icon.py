#!/usr/bin/env python3
"""把 web/favicon.svg 渲染成 Windows 可执行文件使用的多尺寸图标。

用法:
  python tools/make_icon.py web/favicon.svg src/filedrop.ico
"""

from __future__ import annotations

import math
import pathlib
import re
import struct
import sys
import zlib
from xml.etree import ElementTree

# 尺寸越大，每个像素的采样倍数可以略降，以控制生成时间。
SIZES = (16, 24, 32, 48, 64, 256)
PNG_SIZES = {256}

NUM = r"[-+]?(?:\d*\.\d+|\d+\.?)(?:[eE][-+]?\d+)?"
PATH_TOKEN_RE = re.compile(r"[MmZzLlHhVvCcSsQqTtAa]|" + NUM)

NAMED_COLORS = {
    "black": (0, 0, 0),
    "white": (255, 255, 255),
    "red": (255, 0, 0),
    "green": (0, 128, 0),
    "blue": (0, 0, 255),
    "gray": (128, 128, 128),
    "grey": (128, 128, 128),
}


class SvgError(Exception):
    pass


# ---------------------------------------------------------------- 颜色与渐变


def parse_color(text: str) -> tuple[int, int, int]:
    value = text.strip()
    if value.startswith("#"):
        digits = value[1:]
        if len(digits) == 3:
            digits = "".join(ch * 2 for ch in digits)
        if len(digits) != 6:
            raise SvgError(f"unsupported color: {text}")
        return tuple(int(digits[i : i + 2], 16) for i in (0, 2, 4))  # type: ignore[return-value]
    match = re.fullmatch(r"rgba?\(([^)]*)\)", value)
    if match:
        parts = [p.strip() for p in re.split(r"[,\s/]+", match.group(1)) if p.strip()]
        return tuple(int(round(float(p))) for p in parts[:3])  # type: ignore[return-value]
    lowered = value.lower()
    if lowered in NAMED_COLORS:
        return NAMED_COLORS[lowered]
    raise SvgError(f"unsupported color: {text}")


def parse_offset(text: str) -> float:
    return float(text[:-1]) / 100.0 if text.strip().endswith("%") else float(text)


def parse_length(value, default: float = 0.0) -> float:
    if value is None:
        return default
    text = re.sub(r"(px|pt|pc|mm|cm|in)$", "", str(value).strip())
    return float(text) if text else default


def local_name(element: ElementTree.Element) -> str:
    return element.tag.rsplit("}", 1)[-1]


def parse_gradient(element: ElementTree.Element) -> dict:
    if (element.get("gradientUnits") or "objectBoundingBox") != "objectBoundingBox":
        raise SvgError("only objectBoundingBox gradients are supported")

    def fraction(name: str, default: float) -> float:
        raw = element.get(name)
        return default if raw is None else parse_offset(raw)

    stops = []
    for stop in element:
        if local_name(stop) != "stop":
            continue
        color = stop.get("stop-color") or "#000000"
        stops.append((parse_offset(stop.get("offset", "0")), parse_color(color)))
    if not stops:
        raise SvgError("linearGradient without stops")
    stops.sort(key=lambda item: item[0])
    return {
        "x1": fraction("x1", 0.0),
        "y1": fraction("y1", 0.0),
        "x2": fraction("x2", 1.0),
        "y2": fraction("y2", 0.0),
        "stops": stops,
    }


def gradient_color(gradient: dict, u: float, v: float) -> tuple[int, int, int]:
    dx = gradient["x2"] - gradient["x1"]
    dy = gradient["y2"] - gradient["y1"]
    length2 = dx * dx + dy * dy
    if length2 <= 0.0:
        t = 0.0
    else:
        t = ((u - gradient["x1"]) * dx + (v - gradient["y1"]) * dy) / length2
        t = min(1.0, max(0.0, t))
    stops = gradient["stops"]
    if t <= stops[0][0]:
        return stops[0][1]
    for (offset_a, color_a), (offset_b, color_b) in zip(stops, stops[1:]):
        if t <= offset_b:
            span = offset_b - offset_a
            k = 0.0 if span <= 0.0 else (t - offset_a) / span
            return tuple(
                int(round(color_a[i] + (color_b[i] - color_a[i]) * k)) for i in range(3)
            )  # type: ignore[return-value]
    return stops[-1][1]


# ------------------------------------------------------------------- 几何解析


def flatten_cubic(p0, p1, p2, p3, steps: int = 20) -> list[tuple[float, float]]:
    points = []
    for step in range(1, steps + 1):
        t = step / steps
        mt = 1.0 - t
        a = mt * mt * mt
        b = 3.0 * mt * mt * t
        c = 3.0 * mt * t * t
        d = t * t * t
        points.append(
            (
                a * p0[0] + b * p1[0] + c * p2[0] + d * p3[0],
                a * p0[1] + b * p1[1] + c * p2[1] + d * p3[1],
            )
        )
    return points


def flatten_quadratic(p0, p1, p2, steps: int = 14) -> list[tuple[float, float]]:
    points = []
    for step in range(1, steps + 1):
        t = step / steps
        mt = 1.0 - t
        a = mt * mt
        b = 2.0 * mt * t
        c = t * t
        points.append(
            (
                a * p0[0] + b * p1[0] + c * p2[0],
                a * p0[1] + b * p1[1] + c * p2[1],
            )
        )
    return points


def parse_path_data(data: str) -> list[tuple[list[tuple[float, float]], bool]]:
    tokens = PATH_TOKEN_RE.findall(data)
    subpaths: list[tuple[list[tuple[float, float]], bool]] = []
    current: list[tuple[float, float]] = []
    point = (0.0, 0.0)
    start = (0.0, 0.0)
    command = ""
    previous_cubic_control = None
    previous_quadratic_control = None
    index = 0

    def close_subpath() -> None:
        nonlocal current, point
        if current:
            subpaths.append((current, True))
        current = []
        point = start

    def take(count: int) -> list[float]:
        nonlocal index
        values = [float(tokens[index + i]) for i in range(count)]
        index += count
        return values

    def ensure_subpath() -> None:
        nonlocal current
        if not current:
            current = [point]

    while index < len(tokens):
        token = tokens[index]
        if token.isalpha():
            command = token
            index += 1
            if command in "Zz":
                close_subpath()
                continue
        elif not command:
            raise SvgError("path data must start with a command")

        relative = command.islower()
        upper = command.upper()
        ox, oy = point if relative else (0.0, 0.0)

        if upper == "M":
            x, y = take(2)
            point = (ox + x, oy + y)
            if current:
                subpaths.append((current, False))
            start = point
            current = [point]
            command = "l" if relative else "L"
            previous_cubic_control = previous_quadratic_control = None
        elif upper == "L":
            x, y = take(2)
            point = (ox + x, oy + y)
            ensure_subpath()
            current.append(point)
            previous_cubic_control = previous_quadratic_control = None
        elif upper == "H":
            (x,) = take(1)
            point = (ox + x, point[1])
            ensure_subpath()
            current.append(point)
            previous_cubic_control = previous_quadratic_control = None
        elif upper == "V":
            (y,) = take(1)
            point = (point[0], oy + y)
            ensure_subpath()
            current.append(point)
            previous_cubic_control = previous_quadratic_control = None
        elif upper in ("C", "S"):
            if upper == "C":
                x1, y1, x2, y2, x, y = take(6)
                control1 = (ox + x1, oy + y1)
            else:
                x2, y2, x, y = take(4)
                if previous_cubic_control is None:
                    control1 = point
                else:
                    control1 = (
                        2.0 * point[0] - previous_cubic_control[0],
                        2.0 * point[1] - previous_cubic_control[1],
                    )
            control2 = (ox + x2, oy + y2)
            end = (ox + x, oy + y)
            ensure_subpath()
            current.extend(flatten_cubic(point, control1, control2, end))
            point = end
            previous_cubic_control = control2
            previous_quadratic_control = None
        elif upper in ("Q", "T"):
            if upper == "Q":
                qx, qy, x, y = take(4)
                control = (ox + qx, oy + qy)
            else:
                x, y = take(2)
                if previous_quadratic_control is None:
                    control = point
                else:
                    control = (
                        2.0 * point[0] - previous_quadratic_control[0],
                        2.0 * point[1] - previous_quadratic_control[1],
                    )
            end = (ox + x, oy + y)
            ensure_subpath()
            current.extend(flatten_quadratic(point, control, end))
            point = end
            previous_quadratic_control = control
            previous_cubic_control = None
        else:
            raise SvgError(f"unsupported path command: {command}")

    if current:
        subpaths.append((current, False))
    return subpaths


def rounded_rect_subpath(
    x: float, y: float, width: float, height: float, rx: float, ry: float
) -> tuple[list[tuple[float, float]], bool]:
    rx = min(rx, width / 2.0)
    ry = min(ry, height / 2.0)
    if rx <= 0.0 or ry <= 0.0:
        return ([(x, y), (x + width, y), (x + width, y + height), (x, y + height)], True)

    points: list[tuple[float, float]] = []
    corners = (
        (x + width - rx, y + ry, -90.0),
        (x + width - rx, y + height - ry, 0.0),
        (x + rx, y + height - ry, 90.0),
        (x + rx, y + ry, 180.0),
    )
    straight = (
        (x + rx, y),
        (x + width - rx, y),
        (x + width, y + ry),
        (x + width, y + height - ry),
        (x + width - rx, y + height),
        (x + rx, y + height),
        (x, y + height - ry),
        (x, y + ry),
    )
    for index, (cx, cy, start_angle) in enumerate(corners):
        points.append(straight[index * 2])
        for step in range(9):
            angle = math.radians(start_angle + 90.0 * step / 8.0)
            points.append((cx + rx * math.cos(angle), cy + ry * math.sin(angle)))
        points.append(straight[index * 2 + 1])
    return (points, True)


PRESENTATION = (
    "fill",
    "stroke",
    "stroke-width",
    "stroke-linecap",
    "stroke-linejoin",
    "opacity",
    "fill-opacity",
    "stroke-opacity",
)


def parse_shapes(svg_path: pathlib.Path) -> tuple[list[dict], float, float]:
    root = ElementTree.parse(svg_path).getroot()
    view_box = root.get("viewBox")
    if view_box:
        _, _, view_width, view_height = (
            float(value) for value in re.split(r"[\s,]+", view_box.strip())
        )
    else:
        view_width = float(re.sub(r"[a-z%]+$", "", root.get("width", "64")))
        view_height = float(re.sub(r"[a-z%]+$", "", root.get("height", "64")))

    gradients: dict[str, dict] = {}
    for element in root.iter():
        if local_name(element) == "linearGradient":
            gradient_id = element.get("id")
            if gradient_id:
                gradients[gradient_id] = parse_gradient(element)

    shapes: list[dict] = []

    def paint_of(style: dict, key: str):
        raw = (style.get(key) or "none").strip()
        if raw in ("none", "transparent"):
            return None
        if raw.startswith("url("):
            gradient_id = raw[4:].rstrip(")").strip()
            if gradient_id.startswith("#"):
                gradient_id = gradient_id[1:]
            if gradient_id not in gradients:
                raise SvgError(f"unknown gradient: {gradient_id}")
            return ("gradient", gradients[gradient_id])
        return ("solid", parse_color(raw))

    def walk(element: ElementTree.Element, inherited: dict) -> None:
        style = dict(inherited)
        for name in PRESENTATION:
            value = element.get(name)
            if value is not None:
                style[name] = value
            else:
                style.setdefault(name, None)
        tag = local_name(element)
        if tag in ("defs", "title", "desc", "metadata", "linearGradient", "style"):
            return
        if tag == "g":
            for child in element:
                walk(child, style)
            return
        if tag == "svg":
            for child in element:
                walk(child, style)
            return

        for name in ("opacity", "fill-opacity", "stroke-opacity"):
            raw = style.get(name)
            if raw is not None and float(raw) < 1.0:
                raise SvgError(f"unsupported transparency: {name}={raw}")

        shape: dict = {
            "fill": paint_of(style, "fill"),
            "stroke": paint_of(style, "stroke"),
            "stroke_width": parse_length(style.get("stroke-width"), 1.0),
        }
        if tag == "rect":
            shapes_spec = rounded_rect_subpath(
                parse_length(element.get("x")),
                parse_length(element.get("y")),
                parse_length(element.get("width")),
                parse_length(element.get("height")),
                parse_length(element.get("rx", element.get("ry"))),
                parse_length(element.get("ry", element.get("rx"))),
            )
            shape["subpaths"] = [shapes_spec]
        elif tag == "path":
            shape["subpaths"] = parse_path_data(element.get("d") or "")
        else:
            raise SvgError(f"unsupported SVG element: <{tag}>")
        shapes.append(shape)

    walk(root, {})
    return shapes, view_width, view_height


# ---------------------------------------------------------------------- 渲染


def spans_at(polys: list[tuple[list[tuple[float, float]], bool]], yc: float):
    crossings = []
    for points, closed in polys:
        count = len(points)
        limit = count if closed else count - 1
        for index in range(limit):
            x0, y0 = points[index]
            x1, y1 = points[(index + 1) % count]
            if y0 == y1:
                continue
            if (y0 <= yc < y1) or (y1 <= yc < y0):
                crossings.append((x0 + (yc - y0) * (x1 - x0) / (y1 - y0), 1 if y1 > y0 else -1))
    if not crossings:
        return []
    crossings.sort()
    result = []
    winding = 0
    start = 0.0
    for x, direction in crossings:
        was_inside = winding != 0
        winding += direction
        if not was_inside and winding != 0:
            start = x
        elif was_inside and winding == 0:
            result.append((start, x))
    return result


def distance_to_segments(
    px: float, py: float, segments: list[tuple[float, float, float, float]]
) -> float:
    best = float("inf")
    for x0, y0, x1, y1 in segments:
        dx = x1 - x0
        dy = y1 - y0
        length2 = dx * dx + dy * dy
        if length2 <= 0.0:
            distance = math.hypot(px - x0, py - y0)
        else:
            t = ((px - x0) * dx + (py - y0) * dy) / length2
            t = 0.0 if t < 0.0 else (1.0 if t > 1.0 else t)
            distance = math.hypot(px - (x0 + t * dx), py - (y0 + t * dy))
        if distance < best:
            best = distance
    return best


def render(shapes: list[dict], view_width: float, view_height: float, size: int) -> bytearray:
    supersample = 4 if size <= 64 else 2
    samples = size * supersample
    scale_x = samples / view_width
    scale_y = samples / view_height
    buffer = bytearray(samples * samples * 4)
    stride = samples * 4

    for shape in shapes:
        polys = [
            ([(x * scale_x, y * scale_y) for x, y in points], closed)
            for points, closed in shape["subpaths"]
        ]
        all_points = [point for points, _ in polys for point in points]
        if not all_points:
            continue
        min_x = min(point[0] for point in all_points)
        max_x = max(point[0] for point in all_points)
        min_y = min(point[1] for point in all_points)
        max_y = max(point[1] for point in all_points)

        if shape["fill"]:
            row_start = max(0, int(math.floor(min_y - 0.5)))
            row_end = min(samples, int(math.ceil(max_y - 0.5)) + 1)
            paint = shape["fill"]
            if paint[0] == "solid":
                red, green, blue = paint[1]
                pixel = bytes((red, green, blue, 255))
                for row in range(row_start, row_end):
                    spans = spans_at(polys, row + 0.5)
                    base = row * stride
                    for start, end in spans:
                        first = max(0, int(math.ceil(start - 0.5)))
                        last = min(samples, int(math.ceil(end - 0.5)))
                        if last > first:
                            buffer[base + first * 4 : base + last * 4] = pixel * (last - first)
            else:
                gradient = paint[1]
                width = max(max_x - min_x, 1e-9)
                height = max(max_y - min_y, 1e-9)
                for row in range(row_start, row_end):
                    spans = spans_at(polys, row + 0.5)
                    if not spans:
                        continue
                    base = row * stride
                    v = ((row + 0.5) - min_y) / height
                    for start, end in spans:
                        first = max(0, int(math.ceil(start - 0.5)))
                        last = min(samples, int(math.ceil(end - 0.5)))
                        for column in range(first, last):
                            u = ((column + 0.5) - min_x) / width
                            red, green, blue = gradient_color(gradient, u, v)
                            offset = base + column * 4
                            buffer[offset] = red
                            buffer[offset + 1] = green
                            buffer[offset + 2] = blue
                            buffer[offset + 3] = 255

        if shape["stroke"]:
            half = shape["stroke_width"] * max(scale_x, scale_y) / 2.0
            segments = []
            for points, closed in polys:
                count = len(points)
                limit = count if closed else count - 1
                for index in range(limit):
                    x0, y0 = points[index]
                    x1, y1 = points[(index + 1) % count]
                    segments.append((x0, y0, x1, y1))
            red, green, blue = shape["stroke"][1] if shape["stroke"][0] == "solid" else (0, 0, 0)
            solid = bytes((red, green, blue, 255)) if shape["stroke"][0] == "solid" else None
            gradient = shape["stroke"][1] if shape["stroke"][0] == "gradient" else None
            width = max(max_x - min_x, 1e-9)
            height = max(max_y - min_y, 1e-9)
            row_start = max(0, int(math.floor(min_y - half - 0.5)))
            row_end = min(samples, int(math.ceil(max_y + half - 0.5)) + 1)
            column_start = max(0, int(math.floor(min_x - half - 0.5)))
            column_end = min(samples, int(math.ceil(max_x + half - 0.5)) + 1)
            for row in range(row_start, row_end):
                base = row * stride
                py = row + 0.5
                v = (py - min_y) / height
                for column in range(column_start, column_end):
                    px = column + 0.5
                    if distance_to_segments(px, py, segments) > half:
                        continue
                    offset = base + column * 4
                    if solid is not None:
                        buffer[offset : offset + 4] = solid
                    else:
                        red, green, blue = gradient_color(gradient, (px - min_x) / width, v)
                        buffer[offset] = red
                        buffer[offset + 1] = green
                        buffer[offset + 2] = blue
                        buffer[offset + 3] = 255

    return downsample(buffer, supersample, size)


def downsample(buffer: bytearray, supersample: int, size: int) -> bytearray:
    samples = size * supersample
    stride = samples * 4
    total = supersample * supersample
    image = bytearray(size * size * 4)
    for row in range(size):
        target_offsets = [(row * size + column) * 4 for column in range(size)]
        alpha_sums = [0] * size
        red_sums = [0] * size
        green_sums = [0] * size
        blue_sums = [0] * size
        for sample_row in range(supersample):
            base = (row * supersample + sample_row) * stride
            for sample_column in range(supersample):
                start = base + sample_column * 4
                for column in range(size):
                    offset = start + column * supersample * 4
                    alpha = buffer[offset + 3]
                    if not alpha:
                        continue
                    red_sums[column] += buffer[offset]
                    green_sums[column] += buffer[offset + 1]
                    blue_sums[column] += buffer[offset + 2]
                    alpha_sums[column] += alpha
        for column in range(size):
            alpha = alpha_sums[column]
            target = target_offsets[column]
            if not alpha:
                continue
            scale = 255.0 / alpha
            image[target] = min(255, int(round(red_sums[column] * scale)))
            image[target + 1] = min(255, int(round(green_sums[column] * scale)))
            image[target + 2] = min(255, int(round(blue_sums[column] * scale)))
            image[target + 3] = min(255, int(round(alpha / total)))
    return image


# ------------------------------------------------------------------- 编码输出


def encode_png(image: bytearray, size: int) -> bytes:
    def chunk(tag: bytes, payload: bytes) -> bytes:
        return (
            struct.pack(">I", len(payload))
            + tag
            + payload
            + struct.pack(">I", zlib.crc32(tag + payload) & 0xFFFFFFFF)
        )

    rows = b"".join(
        b"\x00" + bytes(image[row * size * 4 : (row + 1) * size * 4]) for row in range(size)
    )
    header = struct.pack(">IIBBBBB", size, size, 8, 6, 0, 0, 0)
    return (
        b"\x89PNG\r\n\x1a\n"
        + chunk(b"IHDR", header)
        + chunk(b"IDAT", zlib.compress(rows, 9))
        + chunk(b"IEND", b"")
    )


def encode_dib(image: bytearray, size: int) -> bytes:
    mask_stride = ((size + 31) // 32) * 4
    pixels = bytearray()
    mask = bytearray()
    for row in range(size - 1, -1, -1):
        for column in range(size):
            offset = (row * size + column) * 4
            pixels += bytes(
                (image[offset + 2], image[offset + 1], image[offset], image[offset + 3])
            )
        bits = bytearray(mask_stride)
        for column in range(size):
            if image[(row * size + column) * 4 + 3] == 0:
                bits[column // 8] |= 0x80 >> (column % 8)
        mask += bits
    header = struct.pack(
        "<IiiHHIIiiII",
        40,
        size,
        size * 2,
        1,
        32,
        0,
        len(pixels) + len(mask),
        0,
        0,
        0,
        0,
    )
    return header + bytes(pixels) + bytes(mask)


def write_ico(images: list[tuple[int, bytearray]], output: pathlib.Path) -> None:
    entries = []
    payloads = []
    for size, image in images:
        data = encode_png(image, size) if size in PNG_SIZES else encode_dib(image, size)
        payloads.append(data)
        entries.append((size, data))

    offset = 6 + 16 * len(entries)
    directory = bytearray(struct.pack("<HHH", 0, 1, len(entries)))
    for size, data in entries:
        directory += struct.pack(
            "<BBBBHHII",
            0 if size >= 256 else size,
            0 if size >= 256 else size,
            0,
            0,
            1,
            32,
            len(data),
            offset,
        )
        offset += len(data)

    output.parent.mkdir(parents=True, exist_ok=True)
    with output.open("wb") as handle:
        handle.write(directory)
        for data in payloads:
            handle.write(data)


def main() -> int:
    if len(sys.argv) != 3:
        print("usage: make_icon.py <favicon.svg> <output.ico>", file=sys.stderr)
        return 2

    svg_path = pathlib.Path(sys.argv[1])
    output = pathlib.Path(sys.argv[2])
    try:
        shapes, view_width, view_height = parse_shapes(svg_path)
        images = [
            (size, render(shapes, view_width, view_height, size)) for size in SIZES
        ]
    except SvgError as error:
        print(f"icon generation failed: {error}", file=sys.stderr)
        return 1

    write_ico(images, output)
    print(f"generated icon {output} ({', '.join(str(size) for size in SIZES)})")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
