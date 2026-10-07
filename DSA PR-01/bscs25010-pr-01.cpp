// ======================= TIME-TRAVEL DEBUGGER - SERVER TEMPLATE =======================

// Pipeline this file implements, top to bottom:
//   0. Receive  -- stream the client's .trace bytes straight to source.bin on disk
//   1. Pass 0X0   -- validity check (FUNC/FUNC_END matching)
//   2. Pass 0X1   -- resolve(): copy EVERY source line into resolve.bin as [offset][size][string], then patch CALL targets.
//   3. Pass 0X2   -- execute resolve.bin: tokenize ONE line at a time, update the call stack, take a snapshot -> Timeline
//   4. Pass 0X3   -- serialize Timeline -> session.tdbg(header + snapshot records + dense index)


#include <iostream>
#include <string>
#include <cstdint>
#include <fstream>
#include <unistd.h>
#include <sys/socket.h>
#include <cstdio>

using namespace std;

// ---- Constants ----
const int32_t MAX_VARS_PER_FRAME = 16;
const int32_t MAX_STACK_DEPTH = 64;
const int32_t MAX_FUNCS = 128;
const int32_t MAX_TOKENS = MAX_VARS_PER_FRAME + 2; // kW + func_name + upto 16 params/args
const int32_t MAX_PATCHES = MAX_FUNCS * 4;
const uint64_t MAX_SOURCE_BYTES = 15ULL * 1024 * 1024; // sanity cap on the declared file length
const int32_t IO_BUFFER_SIZE = 64 * 1024;                  // fixed buffer for streaming to/from disk
const int32_t SOCKET_TIMEOUT_SEC = 5;                      // TODO: apply as SO_RCVTIMEO so a deadclient can't hang the server forever

// ---- Custom data structures

// Stack: back the live Call Stack during execution
template <typename T>
class Stack
{
    struct Node
    {
        T data;
        Node* next;

        Node(const T& val) 
        {
            data = val;
            next = nullptr;
        }
    };
    Node* top;
    int32_t count;

public:
    // Implement these functions:
    Stack()
    {
        top = nullptr;
        count = 0;
    }
    void push(const T& val)
    {

        // pushes the value on the stack if max limit is not reached yet.
        if (count >= MAX_STACK_DEPTH)
            throw std::runtime_error("Stack overflow!");

        Node* temp = new Node(val);
        temp->next = top;
        top = temp;
        count++;

    }
    T pop()
    {
        // pop the top value on the stack

        if (isEmpty())
            throw std::runtime_error("Stack is empty!");

        Node* temp = top;
        T val = top->data;
        top = top->next;
        delete temp;
        count--;

        return val;

    }
    T& peek()
    {
        // returns the top value on the stack
        if(isEmpty())
            throw std::runtime_error("Stack is empty!");

        return top->data;
    }
    bool isEmpty()
    {
        return count == 0;
    }
    int32_t depth()
    {
        return count;
    }
    int32_t snapshot_into(T out[], int32_t maxLen)
    {
        // copies every frame, top to bottom in the array given as a parameter
        // this is what buildSnapshot() call, returns count written

        Node* temp = top;
        int32_t ct = 0;

        while (temp != nullptr && ct < maxLen) 
        {
            out[ct] = temp->data;
            temp = temp->next;
            ct++;
        }
        return ct;
    }
};


// Timeline : doubly linked list of Snapshots
struct Snapshot; // fwd declaration;
struct TimelineNode
{
    Snapshot* data;
    TimelineNode* next;
    TimelineNode* prev;

    TimelineNode(Snapshot* s) 
    {
        data = s;
        next = prev = nullptr;
    }
};

class Timeline
{
    TimelineNode* head, * tail;
    int32_t stepCount;

public:
    // Implement these functions
    Timeline()
    {
        head = tail = nullptr;
        stepCount = 0;
    }
    void record(Snapshot* s)
    {
        // add record in the timeline
        TimelineNode* temp = new TimelineNode(s);

        if (head == nullptr) 
        {
            head = tail = temp;
            stepCount++;
            return;
        }
        temp->prev = tail;
        tail->next = temp;
        tail = temp;
        stepCount++;
    }
    TimelineNode* begin()
    {
        return head;
    }
    int32_t getStepCount()
    {
        return stepCount;
    }
};

// Core structs
struct Variable
{
    string name;
    int32_t value;
};
struct Frame
{
    string func_name;
    int32_t argc;
    Variable argv[MAX_VARS_PER_FRAME];
    int32_t returnLine;
    Variable locals[MAX_VARS_PER_FRAME];
    int32_t localCount;
};
struct Snapshot
{
    Frame callStack[MAX_STACK_DEPTH];
    int32_t stackDepth;
};
struct TTDBHeader
{
    char magic[4]; // "TTDB"
    int32_t version;
    int32_t stepCount;
    int64_t indexOffset;
};
void writeHeader(FILE* f, const TTDBHeader& h)
{
    fwrite(h.magic, 1, 4, f);
    fwrite(&h.version, sizeof(int32_t), 1, f);

    // placeholder for other two data members
}

// resolve.bin - bookkeeping
struct FuncEntry
{
    string funcName;
    int64_t byteOffsetInResolveBin; // where this function's FUNC header record sits
};
struct PendingPatch
{
    int64_t byteOffsetOfOffsetField; // where in resolve.bin to seek back and overwrite
    string targetFuncName;
};



// PASS 0x0: READING source.bin + VALIDITY CHECK
bool readSourceLine(ifstream& in, string& out)
{
    // reads the next nonblank line

    string line{};

    while (getline(in, line))
    {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();

        char empty = 't';

        for (char c : line)
        {
            if (c != ' ' && c != '\t')
            {
                empty = 'f';
                break;
            }
        }

        if (empty == 't')
            continue;

        int32_t start = 0;

        while (start < line.length() &&
            (line[start] == ' ' || line[start] == '\t'))
        {
            start++;
        }

        if (start + 1 < line.length() &&
            line[start] == '/' &&
            line[start + 1] == '/')
        {
            continue;
        }

        out = line;
        return true;
    }

    return false;
}

string firstWord(const string& line)
{
    // returns first word from the input string

    string word{};
    int32_t start = 0;
    int32_t end = -1;
    
    while (start < line.length() && (line[start] == ' ' ||
        line[start] == '\t' || line[start] == '\r' || line[start] == '\n')) 
    {
        start++;
    }
    end = start;

    while (end < line.length() && line[end] != ' ' && line[end] != '\t' && line[end] != '\r' && line[end] != '\n')
    {
        end++;
    }
    word = line.substr(start, end - start);
    return word;
}

string secondWord(const string& line)
{
    // returns the second word
    
    string word{};
    int32_t start = 0;
    int32_t end = -1;

    while (start < line.length() && (line[start] == ' ' || line[start] == '\t' || line[start] == '\r' || line[start] == '\n')) 
    {
        start++;
    }
    end = start;

    while(end < line.length() && line[end] != ' ' && line[end] != '\t' && line[end] != '\r' && line[end] != '\n')
    {
        end++;
    }
    start = end;

    while (start < line.length() && (line[start] == ' ' || line[start] == '\t' || line[start] == '\r' || line[start] == '\n'))
    {
        start++;
    }
    end = start;

    while (end < line.length() && line[end] != ' ' && line[end] != '\t' && line[end] != '\r' && line[end] != '\n')
    {
        end++;
    }

    word = line.substr(start, end - start);

    return word;
}


bool validateProgram(const char* sourcePath)
{
    // for each func defined there should be exactly one func_end and no nested funcs allowed - 

    ifstream fin(sourcePath);
    if (!fin)
    {
        cout << "File not found!\n";
        return false;
    }

    bool insideFunction = false;
    string line{};

    while (readSourceLine(fin, line)) 
    {
        string first = firstWord(line);
        string second = secondWord(line);

        if (first == "func") 
        {
            if (insideFunction == true) 
            {
                cout << "Err: nested functions not allowed!\n";
                return false;
            }

            if (second.empty()) 
            {
                cout << "Err: function name empty not allowed\n";
                return false;
            }

            insideFunction = true;
        }
        else if (first == "func_end") 
        {
            if (insideFunction == false) 
            {
                cout << "Err: ending a func without starting is not allowed!\n";
                return false;
            }

            insideFunction = false;
        }
    }

    if (insideFunction) 
    {
        cout << "Err: a func not ending in the code!\n";
        return false;
    }

    fin.close();

    return true;
}

// PASS 0x1: RESOLVE() -> resolve.bin
int64_t writeResolveRecord(FILE* f, int64_t offsetField, const string& text)
{
    // writes one [offset(8B)][size(4B)][string] record at the current file position
    // returns this record's own starting byte position

    int64_t currentPosition = ftell(f);
    int32_t sizeofString = text.size();

    fwrite(&offsetField, sizeof(int64_t),1,f);
    fwrite(&sizeofString, sizeof(int32_t),1,f);
    fwrite(text.c_str(), sizeof(char),sizeofString,f);

    return currentPosition;
}

int64_t readResolveRecord(FILE* f, string& outText)
{
    // reads one record at the current position and advances past it, returns the offset field - the raw line text comes back untouched in outText.
    if(f == nullptr)
        return -1;

    int64_t offsetField;
    int32_t sizeofString;

    fread(&offsetField,sizeof(int64_t),1,f);
    fread(&sizeofString,sizeof(int32_t),1,f);

    if(sizeofString < 0)
        return -1;

    char* buffer = new char[sizeofString];

    fread(buffer,sizeof(char),sizeofString,f);

    outText = string(buffer,sizeofString);
    delete[] buffer;

    return offsetField;
}


int64_t resolveProgram(const char* sourcePath, const char* resolveBinPath)
{
    FuncEntry funcArray[MAX_FUNCS];
    int32_t funcCount = 0;
    PendingPatch patches[MAX_PATCHES];
    int32_t patchCount = 0;
    // Every source line becomes one record holding the raw line, as-is.
    // resolve() only PEEKS at the leading word(s) -- enough to spot FUNC
    // (remember its position) and CALL (remember which function it needs
    // and where its offset field sits).
    // Once the whole file is written, every CALL's offset field is patched
    // with its target's position. Patching happens after the full write
    // Returns the byte offset of main's FUNC header record.
    // if there is no main return the error 
}

// PASS 0x2: EXECUTION (tokenization happens here)
enum TokenType
{
    KEYWORD,
    IDENTIFIER,
    PARAM
};
struct Token
{
    TokenType type;
    string text;
};
int32_t tokenizeLine(const string& line, Token tokens[], int32_t maxTokens)
{
    // first word is always a instruction keyword
    // instruction set = [func, func_end, call, set, add, sub, mul and div]
    // next word is identifier like name of a function, variable name
    // after identifier all are the params/arg, space separated
}
Snapshot* buildSnapshot(Stack<Frame>& callStack)
{
    // build the snapshot based on the callStack given
}
void executeProgram(const char* resolveBinPath, int64_t mainOffset, Timeline& timeline)
{
    // initialize the call stack
    // make the main frame
    // push main frame on the call stack

    // implementation:
    // execute line by line, and according to the keyword perform action
}

// PASS 0x3: SERIALIZE TIMELINE
void writeTdbg(Timeline& timeline, const char* tdbgPath)
{
    // placeholder for header
    // index array of the size of stepcount from the timeline
    // placing each snapshot in the file while maintaining the index(starting point of each nth snapshot)
    // after timeline add the index array i the file
    // update the header
}
// main section
int32_t main()
{

    if (!validateProgram("source.bin"))
    {
        // send an error response instead of a .tdbg file
        return 1;
    }

    int64_t mainOffset = resolveProgram("source.bin", "resolve.bin");

    Timeline timeline;
    executeProgram("resolve.bin", mainOffset, timeline);

    writeTdbg(timeline, "session.tdbg");

    return 0;
}