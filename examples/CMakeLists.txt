# Offline example — uses MockLLM, so it builds and runs with no API key.
# Real-backend examples (anthropic/ollama, ROS2, game NPC) arrive in later phases.
add_executable(mock_quickstart mock_quickstart.cpp)
target_link_libraries(mock_quickstart PRIVATE corvus::corvus)
