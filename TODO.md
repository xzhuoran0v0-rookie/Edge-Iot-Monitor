# TODO

## Custom Prompt Support

Status: Not implemented.

- Allow users to define or edit the LLM analysis prompt without rebuilding the backend.
- Load a custom prompt template from configuration or a dedicated text file.
- Keep the current built-in English prompt as the fallback.
- Support placeholders such as `{{device_id}}` and `{{recent_sensor_data}}`.
- Preserve the existing JSON response structure.
- Require OLED-facing text to use concise English ASCII characters because the OLED firmware has no Chinese font.
- Validate custom prompts and fall back safely when a prompt is missing or invalid.
- Add tests and usage documentation when this feature is implemented.
