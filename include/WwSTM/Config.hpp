#pragma once

// Verification mode:
// 1 -> prioritize transactional logic validation (system heap + simplified reclamation path)
// 0 -> use full allocator/epoch path.
#ifndef STM_WW_VERIFY_LOGIC_MODE
#define STM_WW_VERIFY_LOGIC_MODE 1
#endif

// Debug logging switch for WwSTM internals.
#ifndef STM_WW_ENABLE_LOGGING
#define STM_WW_ENABLE_LOGGING 0
#endif

