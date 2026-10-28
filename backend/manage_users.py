"""Manage Smart Locker dashboard users.

Usage (run from the backend folder):
    python manage_users.py add <username> <owner|admin|worker>
    python manage_users.py remove <username>
    python manage_users.py list

Passwords are prompted for and stored only as salted hashes in users.json,
which is ignored by Git.
"""

from getpass import getpass
from pathlib import Path
import json
import sys

from werkzeug.security import generate_password_hash

USERS_FILE = Path(__file__).resolve().parent / "users.json"
ROLES = ("owner", "admin", "worker")


def load_users():
    if USERS_FILE.exists():
        return json.loads(USERS_FILE.read_text(encoding="utf-8"))
    return {}


def save_users(users):
    USERS_FILE.write_text(json.dumps(users, indent=2), encoding="utf-8")


def main(argv):
    if len(argv) < 2 or argv[1] not in ("add", "remove", "list"):
        print(__doc__)
        return 1

    users = load_users()
    action = argv[1]

    if action == "list":
        if not users:
            print("No users yet.")
        for name, info in users.items():
            print(f"{name:20} {info['role']}")
        return 0

    if action == "remove":
        if len(argv) != 3 or argv[2] not in users:
            print("Usage: python manage_users.py remove <existing username>")
            return 1
        del users[argv[2]]
        save_users(users)
        print(f"Removed {argv[2]}.")
        return 0

    if len(argv) != 4 or argv[3] not in ROLES:
        print("Usage: python manage_users.py add <username> <owner|admin|worker>")
        return 1
    username, role = argv[2].strip().lower(), argv[3]
    if not username.isalnum():
        print("Username must be letters and digits only.")
        return 1

    password = getpass(f"Password for {username}: ")
    if len(password) < 6:
        print("Password must be at least 6 characters.")
        return 1
    if getpass("Repeat password: ") != password:
        print("Passwords do not match.")
        return 1

    action_word = "Updated" if username in users else "Added"
    users[username] = {"password_hash": generate_password_hash(password), "role": role}
    save_users(users)
    print(f"{action_word} {username} ({role}).")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
