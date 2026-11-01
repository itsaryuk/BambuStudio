#version 140

in vec3 v_position;
in vec3 v_normal;

uniform mat4 view_model_matrix;
uniform mat4 projection_matrix;
uniform mat3 normal_matrix;
// G-code preview section view, (0,0,0,0) never clips.
uniform vec4 u_section_plane;
out float section_dot;

out vec3 eye_normal;

void main()
{
    section_dot = dot(vec4(v_position, 1.0), u_section_plane);
    gl_Position = projection_matrix * view_model_matrix * vec4(v_position, 1.0);
    eye_normal = normal_matrix * v_normal;
}
