// Deterministic per-pixel pattern derived from the pixel position, so the
// oracle checks rasterized geometry and not only a clear.
float4 main(float4 position : SV_Position) : SV_Target
{
    uint2 p = uint2(position.xy);
    uint blue = (p.x * 7 + p.y * 13) & 255;
    return float4(float(p.x * 4), float(p.y * 4), float(blue), 255.0) / 255.0;
}
