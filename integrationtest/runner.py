#!/usr/bin/env python3
"""
Wine Integration Test Runner

This script parses suite.yml configurations and executes both compilation
and integration tests for specified programs.
"""

import argparse
import os
import platform
import subprocess
import sys
import time
from pathlib import Path
from typing import Any, Dict, List, Optional

import yaml


class TestRunner:
    """Main test runner for Wine integration tests."""

    def __init__(self):
        self.verbose = False
        self.dry_run = False
        self.test_results = []
        self.suite_config = None
        self.current_arch = platform.machine().lower()
        self.start_time = None
        self.end_time = None

    def log(self, message: str, level: str = "info"):
        """Log messages with different levels."""
        timestamp = time.strftime("%Y-%m-%d %H:%M:%S")
        if self.verbose or level in ["error", "warning", "success"]:
            print(f"[{timestamp}] [{level.upper()}] {message}")

        if level == "error":
            self.test_results.append(
                {"status": "failed", "message": message, "type": "error"}
            )
        elif level == "success":
            self.test_results.append(
                {"status": "passed", "message": message, "type": "success"}
            )
        elif level == "warning":
            self.test_results.append(
                {"status": "warning", "message": message, "type": "warning"}
            )

    def load_suite_config(self, suite_file: str = "suite.yml") -> bool:
        """Load and parse the suite configuration file."""
        try:
            with open(suite_file, "r") as f:
                self.suite_config = yaml.safe_load(f)

            self.log(
                f"Loaded suite configuration: {self.suite_config['name']}", "success"
            )
            return True

        except FileNotFoundError:
            self.log(f"Suite configuration file not found: {suite_file}", "error")
            return False
        except yaml.YAMLError as e:
            self.log(f"Failed to parse suite configuration: {str(e)}", "error")
            return False
        except Exception as e:
            self.log(f"Unexpected error loading suite configuration: {str(e)}", "error")
            return False

    def run_command(
        self, command: str, cwd: Optional[str] = None, timeout: int = 30
    ) -> Optional[subprocess.CompletedProcess]:
        """Run a shell command and return the result."""
        if self.dry_run:
            self.log(f"[DRY RUN] Would execute: {command}", "warning")
            return subprocess.CompletedProcess(
                args=command, returncode=0, stdout="", stderr=""
            )

        try:
            if self.verbose:
                self.log(f"Executing: {command}", "info")

            result = subprocess.run(
                command,
                shell=True,
                cwd=cwd,
                capture_output=True,
                text=True,
                timeout=timeout,
            )
            return result
        except subprocess.TimeoutExpired:
            self.log(f"Command timed out: {command}", "error")
            return None
        except Exception as e:
            self.log(f"Error running command {command}: {str(e)}", "error")
            return None

    def check_architecture_support(self) -> bool:
        """Check if the current architecture is supported by the suite."""
        if not self.suite_config or "architectures" not in self.suite_config:
            self.log("No architecture requirements specified in suite", "warning")
            return True

        supported_archs = self.suite_config["architectures"]
        if self.current_arch in supported_archs:
            self.log(
                f"Current architecture {self.current_arch} is supported", "success"
            )
            return True
        else:
            self.log(
                f"Current architecture {self.current_arch} is not supported. Supported: {supported_archs}",
                "error",
            )
            return False

    def compile_program(self, program_type: str, program_info: Dict[str, Any]) -> bool:
        """Compile a program based on its type and configuration."""
        if "compilation" not in self.suite_config:
            self.log(
                f"No compilation configuration found for {program_type}", "warning"
            )
            return True

        compilation_config = self.suite_config["compilation"]
        if program_type not in compilation_config:
            self.log(f"No compilation configuration for {program_type} type", "warning")
            return True

        compile_info = compilation_config[program_type]
        if "command" not in compile_info:
            self.log(f"No compilation command specified for {program_type}", "error")
            return False

        self.log(f"Compiling {program_type} program: {program_info['name']}", "info")

        # Replace placeholders in the compilation command
        compile_cmd = compile_info["command"]
        compile_cmd = compile_cmd.replace("{path}", program_info["path"])
        compile_cmd = compile_cmd.replace("{name}", program_info["name"])

        # Determine working directory
        cwd = None
        if "working_dir" in compile_info:
            cwd = compile_info["working_dir"]

        result = self.run_command(compile_cmd, cwd=cwd, timeout=60)

        if result and result.returncode == 0:
            self.log(
                f"Successfully compiled {program_type} program: {program_info['name']}",
                "success",
            )
            return True
        else:
            error_msg = result.stderr if result else "Unknown error"
            self.log(
                f"Failed to compile {program_type} program: {program_info['name']}\nError: {error_msg}",
                "error",
            )
            return False

    def run_integration_test(self, program_info: Dict[str, Any]) -> bool:
        """Run integration tests for a specific program."""
        program_type = program_info["type"]
        program_name = program_info["name"]

        # Find the appropriate test script
        test_script = None
        if program_type == "c":
            test_script = "integrationtest/ping/main.py"
        elif program_type == "rust":
            test_script = "integrationtest/ping/main.py"
        else:
            self.log(
                f"No test script configured for program type: {program_type}", "warning"
            )
            return True

        if not os.path.exists(test_script):
            self.log(f"Test script not found: {test_script}", "error")
            return False

        self.log(
            f"Running integration tests for {program_name} ({program_type})", "info"
        )

        # Run the test script for this specific implementation
        test_cmd = f"python3 {test_script} --implementation {program_type} --verbose"
        result = self.run_command(test_cmd, timeout=120)

        if result and result.returncode == 0:
            self.log(f"Integration tests passed for {program_name}", "success")
            return True
        else:
            error_msg = result.stderr if result else "Unknown error"
            self.log(
                f"Integration tests failed for {program_name}\nError: {error_msg}",
                "error",
            )
            return False

    def run_suite(self, implementations: List[str] = None) -> bool:
        """Run the complete test suite for specified implementations."""
        if not self.suite_config:
            self.log("No suite configuration loaded", "error")
            return False

        if not self.check_architecture_support():
            return False

        self.start_time = time.time()
        self.log(f"Starting test suite: {self.suite_config['name']}", "success")

        if "programs" not in self.suite_config:
            self.log("No programs defined in suite configuration", "error")
            return False

        programs_to_test = []
        for program in self.suite_config["programs"]:
            if implementations and program["type"] not in implementations:
                self.log(
                    f"Skipping {program['name']} (type: {program['type']}) - not in specified implementations",
                    "info",
                )
                continue
            programs_to_test.append(program)

        if not programs_to_test:
            self.log("No programs to test with current filters", "warning")
            return True

        overall_success = True

        # Compile all programs first
        self.log("=== COMPILATION PHASE ===", "info")
        for program in programs_to_test:
            if not self.compile_program(program["type"], program):
                overall_success = False

        # Run integration tests
        self.log("\n=== INTEGRATION TEST PHASE ===", "info")
        for program in programs_to_test:
            if not self.run_integration_test(program):
                overall_success = False

        self.end_time = time.time()

        # Summary
        self.log("\n=== TEST SUMMARY ===", "info")
        elapsed_time = self.end_time - self.start_time
        self.log(f"Test suite completed in {elapsed_time:.2f} seconds", "info")

        passed_tests = sum(1 for r in self.test_results if r["status"] == "passed")
        failed_tests = sum(1 for r in self.test_results if r["status"] == "failed")
        warning_tests = sum(1 for r in self.test_results if r["status"] == "warning")

        self.log(
            f"Results: {passed_tests} passed, {failed_tests} failed, {warning_tests} warnings",
            "info",
        )

        if overall_success:
            self.log("Overall test suite: PASSED ✅", "success")
        else:
            self.log("Overall test suite: FAILED ❌", "error")

        return overall_success


def main():
    """Main entry point for the test runner."""
    parser = argparse.ArgumentParser(
        description="Wine Integration Test Runner",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Examples:
  # Run all tests
  python3 runner.py

  # Run only Rust tests
  python3 runner.py --implementation rust

  # Run only C tests
  python3 runner.py --implementation c

  # Dry run (show what would be executed)
  python3 runner.py --dry-run

  # Verbose output
  python3 runner.py --verbose
    """
    )

    parser.add_argument(
        "--implementation",
        "-i",
        choices=["c", "rust", "both"],
        nargs="*",
        help="Specify which implementations to test (default: both)",
    )
    parser.add_argument(
        "--verbose", "-v", action="store_true", help="Enable verbose output"
    )
    parser.add_argument(
        "--dry-run",
        "-n",
        action="store_true",
        help="Show what would be executed without running",
    )
    parser.add_argument(
        "--suite",
        "-s",
        default="integrationtest/suite.yml",
        help="Path to suite configuration file",
    )

    args = parser.parse_args()

    # Create test runner instance
    runner = TestRunner()
    runner.verbose = args.verbose
    runner.dry_run = args.dry_run

    # Load suite configuration
    if not runner.load_suite_config(args.suite):
        sys.exit(1)

    # Determine implementations to test
    implementations = args.implementation
    if not implementations or "both" in implementations:
        implementations = ["c", "rust"]

    # Run the test suite
    success = runner.run_suite(implementations)

    # Exit with appropriate code
    sys.exit(0 if success else 1)


if __name__ == "__main__":
    main()
