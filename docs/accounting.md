# Usage accounting

How the proxy reads token usage from each upstream protocol and turns it into a
request cost. The cost model, rate representation, and budget windows are in
`README.md`; this file covers only the per-provider usage fields.

- Chat Completions reads DeepSeek's explicit cache-read and cache-miss counts and
  OpenAI's nested `prompt_tokens_details.cached_tokens`. An explicit hit count wins
  over the nested cached count; otherwise the nested count is used. Uncached input
  derives from the inclusive `prompt_tokens` total when it is present, and from the
  reported miss count otherwise. `prompt_tokens_details.cache_write_tokens` is
  charged at the model's cache-write rate, replacing ordinary input pricing for
  those tokens. Reasoning tokens are already included in output.
- Messages input, cache reads, and cache writes are priced separately. Streaming
  combines initial `message_start.message.usage` with later `message_delta.usage`;
  supplied fields replace earlier counts, omitted fields remain, and cumulative
  counts are never added together.
- Responses uses `input_tokens`, `input_tokens_details.cached_tokens`,
  `input_tokens_details.cache_write_tokens`, and `output_tokens`. Reads and writes
  are subsets of total input: writes use their own rate, not an additive charge.
  Reasoning tokens are already included in output and are not added again. JSON uses
  top-level `usage`; SSE reads `response.usage` from `response.completed`,
  `response.incomplete`, and `response.failed`, including unnamed SSE events with a
  JSON `type`. Initial null usage and output-item events do not overwrite the
  request estimate. Malformed optional details are ignored. Cache reads are set to
  zero only when they exceed the input total, and cache writes are limited to the
  uncached remainder. See
  [Responses streaming events](https://developers.openai.com/api/reference/resources/responses/streaming-events)
  and [OpenAI prompt caching](https://developers.openai.com/api/docs/guides/prompt-caching).
- Gemini reads `promptTokenCount` (the inclusive prompt total), the cached subset
  `cachedContentTokenCount`, `toolUsePromptTokenCount`, `candidatesTokenCount`, and
  `thoughtsTokenCount`. Tool-use prompt tokens are added to input, and thoughts are
  added to output alongside candidates. There is no cache-write counter, so cache
  writes are not charged. Cumulative `usageMetadata` objects in SSE events and
  JSON array elements replace the estimate as they grow.
- Usage parsers ignore malformed counters independently instead of rejecting an
  entire usage object: an invalid input count does not discard valid output, or
  vice versa. Missing usage fields default to zero, and incomplete reports can
  underestimate costs; unknown costs do not contribute to budget totals. An
  optional warning identifies the first ignored defect with the user, request ID,
  provider, model, protocol, the offending `field`, `reason`, and the complete JSON
  `usage` value. Request headers, prompts, and response content outside `usage` are
  never included.
