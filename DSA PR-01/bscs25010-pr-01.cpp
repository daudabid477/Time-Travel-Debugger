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
#include <vector>

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
const int64_t MAX_STEPS = 5000;
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
    int64_t returnLine;
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

    fwrite(&h.stepCount, sizeof(int32_t), 1, f);

    fwrite(&h.indexOffset, sizeof(int64_t), 1, f);
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

    // Every source line becomes one record holding the raw line, as-is.
    // resolve() only PEEKS at the leading word(s) -- enough to spot FUNC
    // (remember its position) and CALL (remember which function it needs
    // and where its offset field sits).
    // Once the whole file is written, every CALL's offset field is patched
    // with its target's position. Patching happens after the full write
    // Returns the byte offset of main's FUNC header record.
    // if there is no main return the error 
int64_t resolveProgram(const char* sourcePath, const char* resolveBinPath)
{
    FuncEntry funcArray[MAX_FUNCS];

    int32_t funcCount = 0;

    PendingPatch patches[MAX_PATCHES];

    int32_t patchCount = 0;

    ifstream fin(sourcePath);
    if(!fin)
    {
        cout<<"Err: File not found!\n";
        return -1;
    }

    FILE* resolveBinFile = fopen(resolveBinPath,"wb+");

    if(resolveBinFile == nullptr)
    {
        cout<<"Err: cannot create resolve.bin!\n";
        return -1;
    }

    string line{};
    int64_t currentOffset = 0;

    while(readSourceLine(fin, line))
    {
        string first = firstWord(line);
        string second = secondWord(line);

        int64_t recordPos = writeResolveRecord(resolveBinFile,currentOffset,line);

        currentOffset += 8+4+line.size();

        if(first == "func")
        {
            if(funcCount >= MAX_FUNCS)
            {
                cout<<"Err: Max funcs limit exceeded!\n";
                return -1;
            }
            funcArray[funcCount].funcName = second;
            funcArray[funcCount].byteOffsetInResolveBin = recordPos;

            funcCount++;
        }
        else if(first == "call")
        {
            if(patchCount >= MAX_PATCHES)
            {
                cout<<"Err: Max patch limit exceeded!\n";
                return -1;
            }

            patches[patchCount].byteOffsetOfOffsetField = recordPos;
            patches[patchCount].targetFuncName = second;
            patchCount++;
        }
    }

    for(int32_t i = 0; i< patchCount;i++)
    {
        int64_t targetOffset = -1;

        for(int32_t j =0; j<funcCount; j++)
        {
            if(funcArray[j].funcName == patches[i].targetFuncName)
            {
                targetOffset = funcArray[j].byteOffsetInResolveBin;
                break;
            }
        }

        if(targetOffset == -1)
        {
            cout<<"Err: func not found!\n";
            return -1;
        }

        fseek(resolveBinFile, patches[i].byteOffsetOfOffsetField, SEEK_SET);
        fwrite(&targetOffset,sizeof(int64_t), 1,resolveBinFile);
    }

    int64_t mainOffset = -1;

    for(int32_t i =0;i<funcCount;i++)
    {
        if(funcArray[i].funcName == "main")
        {
            mainOffset = funcArray[i].byteOffsetInResolveBin;
            break;
        }        
    }

    if(mainOffset == -1)
    {
        cout<<"Err: main func not found!\n";
        return -1;
    }

    fclose(resolveBinFile);
    return mainOffset;
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

// first word is always a instruction keyword
    // instruction set = [func, func_end, call, set, add, sub, mul and div]
    // next word is identifier like name of a function, variable name
    // after identifier all are the params/arg, space separated

int32_t tokenizeLine(const string& line, Token tokens[], int32_t maxTokens)
{
    int32_t count = 0;
    int32_t i = 0;

    while (i < line.length() && (line[i] == ' ' || line[i] == '\t'))
    {
        i++;
    }

    while (i < line.length())
    {
        while (i < line.length() &&
            (line[i] == ' ' || line[i] == '\t'))
        {
            i++;
        }

        if (i >= line.length())
            break;

        int32_t start = i;

        while (i < line.length() &&
            line[i] != ' ' && line[i] != '\t')
        {
            i++;
        }

        string word = line.substr(start, i - start);

        if (count >= maxTokens)
            return -1;

        if (count == 0)
        {
            tokens[count].type = KEYWORD;
        }
        else if (count == 1)
        {
            tokens[count].type = IDENTIFIER;
        }
        else
        {
            tokens[count].type = PARAM;
        }

        tokens[count].text = word;
        count++;
    }

    return count;
}


Snapshot* buildSnapshot(Stack<Frame>& callStack)
{
    // build the snapshot based on the callStack given

    Snapshot* snapshot = new Snapshot{};
    snapshot->stackDepth = callStack.snapshot_into(snapshot->callStack,MAX_STACK_DEPTH);
    return snapshot;
}

bool isNumber(const string& number)
{
    if (number.empty())
        return false;

    size_t i = (number[0] == '-') ? 1 : 0;

    if (i == number.size())               
        return false;

    for (; i < number.size(); i++)
    {
        if (number[i] < '0' || number[i] > '9')
            return false;
    }

    return true;
}

int32_t parseInteger(const string& integer)
{
    try
    {
        return stoi(integer);
    }
    catch (...)
    {
        throw runtime_error("Err: invalid integer: " + integer);
    }
}

Variable* findVariable(Frame& frame, const string& name)
{
    for (int32_t i = 0; i < frame.argc; i++)
    {
        if (frame.argv[i].name == name)
            return &frame.argv[i];
    }

    for (int32_t i = 0; i < frame.localCount; i++)
    {
        if (frame.locals[i].name == name)
            return &frame.locals[i];
    }

    return nullptr;
}

int32_t getVariable(Frame& frame, const string& name)
{
    Variable* v = findVariable(frame, name);

    if (v == nullptr)
        throw runtime_error("Err: variable not found: " + name);

    return v->value;
}

void setVariable(Frame& frame, const string& name, int32_t value)
{
    if (isNumber(name))
        throw runtime_error("Err: cannot assign to a number: " + name);

    Variable* v = findVariable(frame, name);

    if (v != nullptr)
    {
        v->value = value;
        return;
    }

    if (frame.localCount >= MAX_VARS_PER_FRAME)
        throw runtime_error("Err: too many local variables!");

    frame.locals[frame.localCount].name = name;
    frame.locals[frame.localCount].value = value;
    frame.localCount++;
}

int32_t getOperand(Frame& frame, const string& text)
{
    if (isNumber(text))
        return parseInteger(text);

    return getVariable(frame, text);
}

int32_t checkedArithmetic(int32_t current, int32_t amount, const string& operation)
{
    int64_t result;

    if (operation == "add")
        result = (int64_t)current + amount;
    else if (operation == "sub")
        result = (int64_t)current - amount;
    else if (operation == "mul")
        result = (int64_t)current * amount;
    else if (operation == "div")
    {
        if (amount == 0)
            throw runtime_error("Err: division by zero!");

        result = (int64_t)current / amount;
    }
    else
        throw runtime_error("Err: unknown operation!");


    return (int32_t)result;
}


void executeProgram(const char* resolveBinPath, int64_t mainOffset, Timeline& timeline)
{
    FILE* file = fopen(resolveBinPath, "rb");
    if (file == nullptr)
        throw runtime_error("Err: cannot open resolve file!");

    try
    {
        Stack<Frame> callStack;

        string argSources[MAX_STACK_DEPTH][MAX_VARS_PER_FRAME];


        Frame mainFrame{};
        mainFrame.func_name = "main";
        mainFrame.returnLine = -1;
        callStack.push(mainFrame);

        fseek(file, mainOffset, SEEK_SET);
        string line;
        if (readResolveRecord(file, line) == -1)
            throw runtime_error("Err: cannot read main!");

        int64_t steps = 0;
        while (true)
        {
            steps++;
            if (steps > MAX_STEPS)
                throw runtime_error("Err: step limit exceeded!\n");

            int64_t storedOffset = readResolveRecord(file, line);
            if (storedOffset == -1)
                throw runtime_error("Err: unexpected end of resolve file\n!");

            int64_t nextPosition = ftell(file);   

            Token tokens[MAX_TOKENS];
            int32_t count = tokenizeLine(line, tokens, MAX_TOKENS);

            if (count < 0)
                throw runtime_error("Err: too many parameters!\n");
            if (count == 0)
                continue;

            string instruction = tokens[0].text;
            Frame& current = callStack.peek();

            if (instruction == "set")
            {
                if (count != 3)
                    throw runtime_error("Err: invalid set instruction!");

                setVariable(current, tokens[1].text, getOperand(current, tokens[2].text));
            }
            else if (instruction == "add" || instruction == "sub" ||
                     instruction == "mul" || instruction == "div")
            {
                if (count != 3)
                    throw runtime_error("Err: invalid arithmetic instruction!");

                int32_t currentValue = getVariable(current, tokens[1].text);   // must already exist
                int32_t amount = getOperand(current, tokens[2].text);
                int32_t result = checkedArithmetic(currentValue, amount, instruction);

                setVariable(current, tokens[1].text, result);
            }
            else if (instruction == "call")
            {
                if (count < 2)
                    throw runtime_error("Err: invalid call instruction!");
                if (callStack.depth() >= MAX_STACK_DEPTH)
                    throw runtime_error("Err: stack overflow (recursion too deep)!");

                
                fseek(file, storedOffset, SEEK_SET);
                string header;
                if (readResolveRecord(file, header) == -1)
                    throw runtime_error("Err: call target not found!");

                Token headerTokens[MAX_TOKENS];
                int32_t headerCount = tokenizeLine(header, headerTokens, MAX_TOKENS);

                if (headerCount - 2 != count - 2)
                    throw runtime_error("Err: wrong argument count for '" + tokens[1].text + "'");

                int32_t newIndex = callStack.depth();

                Frame newFrame{};
                newFrame.func_name = tokens[1].text;
                newFrame.argc = count - 2;
                newFrame.returnLine = (int64_t)nextPosition;   

                for (int32_t i = 0; i < newFrame.argc; i++)
                {
                    newFrame.argv[i].name = headerTokens[2 + i].text;                    
                    newFrame.argv[i].value = getOperand(current, tokens[2 + i].text);    
                    argSources[newIndex][i] = tokens[2 + i].text;                       
                }

                callStack.push(newFrame);   
            }
            else if (instruction == "func_end")
            {
                if (callStack.depth() == 1)
                {
                    timeline.record(buildSnapshot(callStack));
                    break;
                }

                int32_t finishedIndex = callStack.depth() - 1;   
                Frame finished = callStack.pop();
                Frame& caller = callStack.peek();

                for (int32_t i = 0; i < finished.argc; i++)
                {
                    Variable* v = findVariable(caller, argSources[finishedIndex][i]);
                    if (v != nullptr)
                        v->value = finished.argv[i].value;
                }


                fseek(file, finished.returnLine, SEEK_SET);
            }
            else
            {
                throw runtime_error("Err: unknown instruction: " + instruction);
            }


            timeline.record(buildSnapshot(callStack));
        }
    }
    catch (...)
    {
        fclose(file);
        throw;   
    }

    fclose(file);
}



//////////////////////////////////////////////////////////////////////////////////////////////////
bool writeString(FILE* file, const string& text)
{
    int32_t length = static_cast<int32_t>(text.size());

    if (fwrite(&length, sizeof(int32_t), 1, file) != 1)
    {
        return false;
    }

    if (length > 0)
    {
        if (fwrite(text.data(), sizeof(char), length, file)
            != static_cast<size_t>(length))
        {
            return false;
        }
    }

    return true;
}


bool writeVariable(FILE* file, const Variable& var)
{
    if (!writeString(file, var.name))
    {
        return false;
    }

    if (fwrite(&var.value, sizeof(int32_t), 1, file) != 1)
    {
        return false;
    }

    return true;
}



bool writeFrame(FILE* file, const Frame& frame)
{
    if (!writeString(file, frame.func_name))
    {
        return false;
    }

    if (fwrite(&frame.argc, sizeof(int32_t), 1, file) != 1)
    {
        return false;
    }

    for (int32_t i = 0; i < frame.argc; i++)
    {
        if (!writeVariable(file, frame.argv[i]))
        {
            return false;
        }
    }

    if (fwrite(&frame.returnLine, sizeof(int64_t), 1, file) != 1)
    {
        return false;
    }

    if (fwrite(&frame.localCount, sizeof(int32_t), 1, file) != 1)
    {
        return false;
    }

    for (int32_t i = 0; i < frame.localCount; i++)
    {
        if (!writeVariable(file, frame.locals[i]))
        {
            return false;
        }
    }

    return true;
}




bool writeSnapshot(FILE* file, const Snapshot& snapshot)
{
    if (fwrite(&snapshot.stackDepth, sizeof(int32_t), 1, file) != 1)
    {
        return false;
    }

    for (int32_t i = 0; i < snapshot.stackDepth; i++)
    {
        if (!writeFrame(file, snapshot.callStack[i]))
        {
            return false;
        }
    }

    return true;
}


bool writeTimeline(FILE* file, Timeline& timeline, int64_t offsets[], int32_t maxSteps)
{
    TimelineNode* current = timeline.begin();
    int32_t step = 0;

    while (current != nullptr)
    {
        if (step >= maxSteps)
        {
            return false;
        }

        offsets[step] = ftell(file);

        if (!writeSnapshot(file, *current->data))
        {
            return false;
        }

        step++;
        current = current->next;
    }

    return step == timeline.getStepCount();
}


bool writeDenseIndex(FILE* file, const int64_t offsets[], int32_t stepCount)
{
    for (int32_t i = 0; i < stepCount; i++)
    {
        if (fwrite(&offsets[i], sizeof(int64_t), 1, file) != 1)
        {
            return false;
        }
    }

    return true;
}

// PASS 0x3: SERIALIZE TIMELINE
void writeTdbg(Timeline& timeline, const char* tdbgPath)
{
    FILE* f = fopen(tdbgPath, "wb+");

    if (f == nullptr)
        throw runtime_error("Err: cannot create tdbg file!");

    int32_t steps = timeline.getStepCount();
    vector<int64_t> offsets(steps);          

    TTDBHeader h{};
    h.magic[0] = 'T';
    h.magic[1] = 'T';
    h.magic[2] = 'D';
    h.magic[3] = 'B';
    h.version = 1;
    h.stepCount = steps;
    h.indexOffset = 0;                     

    
    writeHeader(f, h);

    bool success = writeTimeline(f, timeline, offsets.data(), steps);

    
    if (success)
    {
        h.indexOffset = ftell(f);            
        success = writeDenseIndex(f, offsets.data(), steps);
    }

    
    if (success)
    {
        fseek(f, 0, SEEK_SET);
        writeHeader(f, h);
    }

    fclose(f);

    if (!success)
        throw runtime_error("Err: failed writing tdbg file!");
}


int32_t main()
{

    if (!validateProgram("source.bin"))
    {
        cout<<"Err: invlaid code in your source file!\n";
        // send an error response instead of a .tdbg file
        return 1;
    }

    int64_t mainOffset = resolveProgram("source.bin", "resolve.bin");

    Timeline timeline;
    executeProgram("resolve.bin", mainOffset, timeline);


    writeTdbg(timeline, "session.tdbg");

    return 0;
}