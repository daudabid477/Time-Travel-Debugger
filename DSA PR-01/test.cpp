
void executeProgram(const char* resolveBinPath, int64_t mainOffset, Timeline& timeline)
{
    FILE* resolveBinFile = fopen(resolveBinPath, "rb");
    if(resolveBinFile == nullptr)
    {
        cout<<"Err: File not found!\n";
        return;
    }

    Stack<Frame> callStack;

    Frame mainFrame{};
    mainFrame.func_name = "main";
    mainFrame.argc = 0;
    mainFrame.returnLine = -1;
    mainFrame.localCount = 0;

    callStack.push(mainFrame);

    if(fseek(resolveBinFile, mainOffset, SEEK_SET) != 0)
    {
        cout<<"Err: cannot go to main!\n";
        return;
    }

    string line;

    while(true)
    {
        int64_t storedOffset = readResolveRecord(resolveBinFile, line);

        if(storedOffset == -1)
        {
            break;
        }
        int64_t nextPosition = ftell(resolveBinFile);

        Token tokens[MAX_TOKENS];
        int32_t tokenCount = tokenizeLine(line, tokens, MAX_TOKENS);

        if(tokenCount <= 0)
        {
            continue;
        }
        
        if(tokens[0].text == "func_end")
        {
            if(callStack.depth() == 1)
            {
                Snapshot* currentSnapshot = buildSnapshot(callStack);
                timeline.record(currentSnapshot);

                break;
            }
            callStack.pop();
            int64_t returnPos = callStack.peek().returnLine;

            if(fseek(resolveBinFile, returnPos, SEEK_SET) != 0)
            {
                throw runtime_error("Err: cannot return to caller! \n");
            }
            
            Snapshot* currentSnapshot = buildSnapshot(callStack);
            timeline.record(currentSnapshot);
            
            continue;
        }


        
        if(tokens[0].text == "set")
        {
            if(tokenCount != 3)
            {
                throw runtime_error("Err: invalid set instruction!");
            }

        int32_t value = stoi(tokens[2].text);

        setVariable(callStack.peek(), tokens[1].text, value);
        }
        else if(tokens[0].text == "add")
        {
            if(tokenCount != 3)
            {
                throw runtime_error("Err: invalid add instruction!");
            }

            string variableName = tokens[1].text;
            int32_t currentValue =
            getVariable(callStack.peek(), variableName);

            int32_t amount = stoi(tokens[2].text);

            setVariable(callStack.peek(),variableName,currentValue + amount);
        }
        
        else if(tokens[0].text == "sub")
        {
            if(tokenCount != 3)
            {
                throw runtime_error("Err: invalid sub instruction!");
            }

            string variableName = tokens[1].text;

            int32_t currentValue =
            getVariable(callStack.peek(), variableName);

            int32_t amount = stoi(tokens[2].text);

            setVariable(callStack.peek(),variableName,currentValue - amount);
        }
        else if(tokens[0].text == "mul")
        {
            if(tokenCount != 3)
            {
                throw runtime_error("Err: invalid mul instruction!");
            }

            string variableName = tokens[1].text;

            int32_t currentValue =
            getVariable(callStack.peek(), variableName);

            int32_t amount = stoi(tokens[2].text);

            setVariable(callStack.peek(),variableName,currentValue * amount);
        }
        else if(tokens[0].text == "div")
        {
            if(tokenCount != 3)
            {
                throw runtime_error("Err: invalid div instruction!");
            }

            string variableName = tokens[1].text;

            int32_t currentValue =
            getVariable(callStack.peek(), variableName);

            int32_t amount = stoi(tokens[2].text);

            if(amount == 0)
            {
                throw runtime_error("Err: division by zero!");
            }

            setVariable(callStack.peek(),variableName,currentValue / amount);
        }
        else if (op == "call")
        {
            if (n < 2)
            {
                throw runtime_error("Err: call has no func to call!\n");
            }

            if (callStack.depth() >= MAX_STACK_DEPTH)
            {
                throw runtime_error("Err: Limit exceeds! stack overflow! \n");
            }

            fseek(file, storedOffset, SEEK_SET);
            string header;

            if (readResolveRecord(file, header) == -1)
            {
                throw runtime_error("Err: call target func not found!\n");
            }
            int64_t bodyOffset = ftell(file);

            Token headerToken[MAX_TOKENS];
            int32_t headerCount = tokenizeLine(header, headerToken, MAX_TOKENS);
            if (headerCount < 2 || headerCount - 2 != n - 2)
                throw runtime_error("Err: wrong argument count for '" + tokens[1].text + "'");

            Frame nf{};
            nf.func_name = tokens[1].text;
            nf.argc = n - 2;
            nf.returnLine = (int32_t)nextOffset;
            for (int32_t i = 0; i < nf.argc; i++)
            {
                nf.argv[i].name = ht[2 + i].text;
                nf.argv[i].value = getValue(cur, tokens[2 + i].text);
                nf.argSourceNames[i] = tokens[2 + i].text;
            }
            callStack.push(nf);        
            currentOffset = bodyOffset;
        }

        }

        Snapshot* currentSnapshot = buildSnapshot(callStack);
        timeline.record(currentSnapshot);

    }
}
