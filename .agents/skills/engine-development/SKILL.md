---
name: bellwether-engine-development
description: Implement, test and review Bellwether Engine components, renderer passes, editor commands and Lua APIs.
---

# Bellwether engine development

1. Read the workspace and Engine AGENTS.md files and the current production plan.
2. Inspect the affected implementation, feature scene and tests inside this workspace only.
3. Implement the real feature using the chosen stack and Hazel naming. Keep editor, runtime and agent command semantics identical.
4. Add positive and negative unit tests. Add the component/API to the feature scene and test script. Verify save/load, invalid handles and teardown where relevant.
5. Configure and build; run CTest. If GPU code changed, run the Vulkan validation/capture scenario and inspect its output. Record exact commands, source revision, device and limitations.
6. Review the final diff for correctness, lifetime, rollback, portability and style; fix findings and rerun affected checks before committing.
7. Update the production plan with implemented, tested and pending states. Do not convert a planned item into a claim of readiness.
