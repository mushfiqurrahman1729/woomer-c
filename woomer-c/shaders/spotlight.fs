#version 330

// Input vertex attributes (from vertex shader)
in vec2 fragTexCoord;
in vec4 fragColor;

// Input uniform values
uniform sampler2D texture0;
uniform vec4 colDiffuse;

uniform vec4 spotlightTint;
uniform vec2 cursorPosition;
uniform float spotlightRadiusMultiplier;

// This is multiplied by spotlightRadiusMultiplier to obtain the radius in pixels
const int UNIT_RADIUS = 60;

// Output fragment color
out vec4 finalColor;

void main()
{
    // Texel color fetching from texture sampler
    vec4 texelColor = texture(texture0, fragTexCoord);

    // Calculate distance from the current fragment to the cursor position
    float distanceToCursor = distance(gl_FragCoord.xy, vec2(cursorPosition.x, cursorPosition.y));

    // Calculate spotlight radius in pixels
    float spotlightRadius = float(UNIT_RADIUS) * spotlightRadiusMultiplier;

    // 0 inside the circle, 1 outside, with a ~1.5px anti-aliased edge so the
    // rim doesn't shimmer while the radius is animating
    float outside = smoothstep(spotlightRadius - 0.75, spotlightRadius + 0.75, distanceToCursor);

    vec4 dimmed = mix(texelColor, vec4(spotlightTint.rgb, 1.0), spotlightTint.a);
    finalColor = mix(texelColor, dimmed, outside) * colDiffuse;
}
