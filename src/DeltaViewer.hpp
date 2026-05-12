#ifndef DELTA_VIEWER_HPP
#define DELTA_VIEWER_HPP

#include <filesystem>

/**
 * Inspect a binary delta file and print a human-readable summary of its
 * chunk records to stdout.
 *
 * @param[in] delta_file path to the delta file to inspect
 * @return 0 on success, non-zero on read/parse errors
 */
int view_delta(const std::filesystem::path& delta_file);

#endif // DELTA_VIEWER_HPP
