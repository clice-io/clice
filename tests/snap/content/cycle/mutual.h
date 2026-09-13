#pragma once

struct Node;

struct List {
    Node* head;
};

struct Node {
    List* owner;
    Node* next;
};

int pong(int n);

inline int ping(int n) { return n ? pong(n - 1) : 0; }

inline int pong(int n) { return n ? ping(n - 1) : 1; }
