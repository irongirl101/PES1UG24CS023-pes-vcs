**Screenshot 1A:** Output of `./test_objects` showing all tests passing.

![image1](images/image1)

**Screenshot 1B:** `find .pes/objects -type f` showing the sharded directory structure.

![image2](images/image2)

**Screenshot 2A:** Output of `./test_tree` showing all tests passing.

![image3](images/image3)

**Screenshot 2B:** Pick a tree object from `find .pes/objects -type f` and run `xxd .pes/objects/XX/YYY... | head -20` to show the raw binary format.

![image4](images/image4)




**Q5.1:** A branch in Git is just a file in `.git/refs/heads/` containing a commit hash. Creating a branch is creating a file. Given this, how would you implement `pes checkout <branch>` — what files need to change in `.pes/`, and what must happen to the working directory? What makes this operation complex?

At a high level, checkout does two things:

Move HEAD to point to the new branch
Rewrite the working directory to match that branch’s commit tree
What changes inside .pes/?

Update from:
ref: refs/heads/main
to:
ref: refs/heads/<branch>
.pes/refs/heads/<branch>

This already exists (contains commit hash). You just read from it.
.pes/index
Must be rewritten to match the target commit’s tree
Essentially: staging area becomes identical to that commit

### What happens to the working directory?

You need to:

Load the commit hash from the branch
Read its tree object
Recursively:
Create/update files from blobs
Create directories from trees
Remove files that exist in current working dir but not in target tree

So effectively:

Working directory = exact snapshot of target commit

### Why is checkout complex?

Because it’s not just file copying:

You must avoid overwriting uncommitted changes
You must handle:
file → directory transitions
directory → file transitions
You must sync:
working directory
index
HEAD

**Q5.2:** When switching branches, the working directory must be updated to match the target branch's tree. If the user has uncommitted changes to a tracked file, and that file differs between branches, checkout must refuse. Describe how you would detect this "dirty working directory" conflict using only the index and the object store.

You are restricted to:

index
object store

No shortcuts like filesystem timestamps.

Core idea

You detect conflicts by comparing three versions of a file:

Source	Meaning
Working directory	What user currently has
Index	Last staged version
Target branch	What checkout wants to write
Conflict condition

A conflict exists if:

File is modified in working directory vs index
AND file is different in target branch vs index
How to implement

For each tracked file:

Step 1 — Check if working directory is modified
Read file from disk
Hash it → compare with index entry hash

If different → user has uncommitted changes

Step 2 — Check if branch would overwrite it
Get blob hash for same path in target commit
Compare with index hash

If different → branch has a different version

Final condition
if (working_dir_hash != index_hash) AND (target_hash != index_hash):
    conflict → refuse checkout
Why this works
Index = common ancestor baseline
Detects whether:
user changed it
branch changed it
If both changed → overwrite would lose data

**Q5.3:** "Detached HEAD" means HEAD contains a commit hash directly instead of a branch reference. What happens if you make commits in this state? How could a user recover those commits?
What it means

Instead of:

HEAD → refs/heads/main → commit

You have:

HEAD → commit hash directly
What happens when you commit?

New commits:

Do not belong to any branch
Form a chain like:
A -- B -- C   (main)
       \
        D -- E   (detached HEAD)

But:

No branch points to D or E
Danger

These commits are:

unreachable (eventually garbage-collected)

How to recover

Before GC runs:

Option 1 — Create a branch
pes branch rescue

This makes:

refs/heads/rescue → E

**Q6.1:** Over time, the object store accumulates unreachable objects — blobs, trees, or commits that no branch points to (directly or transitively). Describe an algorithm to find and delete these objects. What data structure would you use to track "reachable" hashes efficiently? For a repository with 100,000 commits and 50 branches, estimate how many objects you'd need to visit.
Step 1 — Find reachable objects

Start from all roots:

All branch heads (refs/heads/*)
(optionally HEAD)
Traverse graph

Use:

DFS or BFS

For each commit:

Mark commit
Visit its tree
Visit parent commits

For each tree:

Visit blobs
Visit subtrees
Data structure

Use:

Hash set (unordered set)

Stores all reachable object hashes

Why?

O(1) lookup
Avoid revisiting
Step 2 — Sweep
Iterate over .pes/objects/
Delete objects not in reachable set
How many objects?

For:

100,000 commits
50 branches

Important insight:

Branches mostly overlap history

So traversal ≠ 50 × 100k

Instead:

Likely ~100k commits total
Trees + blobs maybe:
~2–5× commits depending on repo

So:

Rough estimate: 200k–500k objects visited

**Q6.2:** Why is it dangerous to run garbage collection concurrently with a commit operation? Describe a race condition where GC could delete an object that a concurrent commit is about to reference. How does Git's real GC avoid this?
Race scenario
Thread 1 (commit):
Creates blob object
Creates tree object referencing blob
Creates commit referencing tree
Has not yet updated branch ref
Thread 2 (GC):
Starts traversal from current refs
Does NOT see new commit/tree/blob (not referenced yet)
Marks them as unreachable
Deletes them
Back to Thread 1:
Updates branch → now points to commit
But objects are gone

Repo is corrupted

Why this happens

Because:

Object creation and reference update are not atomic

How real Git avoids this

Git uses multiple strategies:

1. Write objects first, refs last
Objects exist before being referenced
But GC must not delete "new but not yet referenced" objects
2. Grace period (important!)
Objects are not deleted immediately
Git keeps:
recent unreachable objects
Only deletes after time threshold
3. Reflog protection
Even if branch moves:
old commits are still referenced in reflog
GC treats them as reachable
4. Locking refs
Updates to refs are atomic
Prevent inconsistent states
5. Packfiles + marking phase
GC works on a snapshot
Doesn’t interfere with ongoing writes
