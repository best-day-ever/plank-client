#extension GL_OES_EGL_image_external : require
precision highp float;

varying vec2 vTexCoord;

uniform samplerExternalOES uTexture;
uniform int uIdentityGbr8;
uniform int uPackedBt709;

void main() {
    vec4 sample = texture2D(uTexture, vTexCoord);
    if (uPackedBt709 != 0) {
        // Raw Y410 imported as XR30 samples R:G:B = V:Y:U. No implicit
        // driver YUV conversion. Full-range 10-bit chroma is centered at
        // code 512, not 511.5. BT.709 is applied once in high precision;
        // the encoded sRGB transfer is retained (this is not HDR).
        float y = sample.g;
        float cb = sample.b - 512.0 / 1023.0;
        float cr = sample.r - 512.0 / 1023.0;
        gl_FragColor = vec4(y + 1.5748 * cr,
                           y - 0.1873242729 * cb - 0.4681242729 * cr,
                           y + 1.8556 * cb, 1.0);
        return;
    }
    gl_FragColor = uIdentityGbr8 != 0 ? vec4(sample.b, sample.r, sample.g, 1.0) : sample;
}
