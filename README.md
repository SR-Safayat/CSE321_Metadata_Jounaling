# 🗄️ Metadata Journaling Simulation (CSE321)

A systems-level project demonstrating the implementation of metadata journaling, a critical file system technique used to prevent data corruption during crashes or power failures.

## ✨ Overview
In modern file systems (like ext3/ext4), journaling ensures data integrity. This project simulates the journaling process by logging metadata operations (like file creation, deletion, and modification) to a dedicated journal area before committing them to the main file system. If an interruption occurs, the journal is read to recover the file system state.

## 📂 Repository Structure
* `src/` - Contains the core logic and source code for the journaling system.
* `tests/` - Test cases, dummy files, and crash simulation scripts.
* `docs/` - Project documentation and architecture details.

## ⚙️ Concepts Demonstrated
* File System Structures (Superblocks, Inodes, Data Blocks)
* Write-Ahead Logging (WAL)
* Crash Recovery Mechanisms
* Transaction Commits and Checkpointing

## 🚀 How to Compile and Run
*(Assuming a standard C/Makefile setup - adjust if using Python or another language)*

