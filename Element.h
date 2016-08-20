#pragma once

#include "Codec.h"
#include "InPin.h"
#include "OutPin.h"

#include <pthread.h>
#include <sys/time.h>


// Push model
//
// source.Executre()
// source.Connect(sink)
//										->	sink.AcceptConnection(source)
//
// while(IsRunning())
//										->	sink.ProcessBuffer(buffer)
//		source.AcceptProcessedBuffer()	<-
// loop
//
// source.Connect(nullptr)				->  sink.Disconnect(source)
//




template <typename T>	// where T : Pin*
class PinCollection
{
	friend class Element;

	std::vector<T> pins;

	
protected:
	
	void Add(T value)
	{
		pins.push_back(value);
	}

	void Clear()
	{
		pins.clear();
	}


public:

	PinCollection()
	{
	}


	int Count() const
	{
		return pins.size();
	}

	T Item(int index)
	{
		if (index < 0 || index > pins.size())
			throw ArgumentOutOfRangeException();

		return pins[index];
	}

	void Flush()
	{
		for (auto pin : pins)
		{
			pin->Flush();
		}
	}
};

class InPinCollection : public PinCollection<InPinSPTR>
{
public:
	InPinCollection()
		: PinCollection()
	{
	}
};

class OutPinCollection : public PinCollection<OutPinSPTR>
{
public:
	OutPinCollection()
		: PinCollection()
	{
	}
};


enum class ExecutionState
{
	WaitingForExecute = 0,
	Initializing,
	Executing,
	Terminating
};

class Element : public std::enable_shared_from_this<Element>
{
	Thread thread = Thread(std::function<void()>(std::bind(&Element::InternalWorkThread, this)));
	MediaState state = MediaState::Pause;
	InPinCollection inputs;
	OutPinCollection outputs;
	ExecutionState status = ExecutionState::WaitingForExecute;

	pthread_cond_t waitCondition = PTHREAD_COND_INITIALIZER;
	pthread_mutex_t waitMutex = PTHREAD_MUTEX_INITIALIZER;
	bool canSleep = true;

	std::string name = "Element";

	pthread_cond_t executionWaitCondition = PTHREAD_COND_INITIALIZER;
	pthread_mutex_t executionWaitMutex = PTHREAD_MUTEX_INITIALIZER;

	bool logEnabled = false;


	void SetExecutionState(ExecutionState state)
	{
		status = state;

		pthread_mutex_lock(&executionWaitMutex);
		pthread_cond_signal(&executionWaitCondition);
		pthread_mutex_unlock(&executionWaitMutex);
	}

protected:
	Element()
	{
	}


	
	virtual void Initialize()
	{
	}

	virtual void DoWork()
	{
		Log("Element (%s) DoWork exited.\n", name.c_str());
	}

	virtual void Flush()
	{
		inputs.Flush();
		outputs.Flush();

		Log("Element (%s) Flush exited.\n", name.c_str());
	}

	void InternalWorkThread()
	{
		Log("Element (%s) InternalWorkThread entered.\n", name.c_str());

		SetExecutionState(ExecutionState::Initializing);
		Initialize();

		SetExecutionState(ExecutionState::Executing);
		while (status == ExecutionState::Executing)
		{
			if (state == MediaState::Play)
			{
				DoWork();
			}

			Log("Element (%s) InternalWorkThread sleeping.\n", name.c_str());

			pthread_mutex_lock(&waitMutex);

			while (canSleep)
			{
				pthread_cond_wait(&waitCondition, &waitMutex);
			}

			canSleep = true;

			pthread_mutex_unlock(&waitMutex);

			Log("Element (%s) InternalWorkThread woke.\n", name.c_str());


		}

		SetExecutionState(ExecutionState::WaitingForExecute);

		Log("Element (%s) InternalWorkThread exited.\n", name.c_str());
	}


	void AddInputPin(InPinSPTR pin)
	{
		inputs.Add(pin);
	}
	void ClearInputPins()
	{
		inputs.Clear();
	}

	void AddOutputPin(OutPinSPTR pin)
	{
		outputs.Add(pin);
	}
	void ClearOutputPins()
	{
		outputs.Clear();
	}




public:

	InPinCollection* Inputs()
	{
		return &inputs;
	}
	OutPinCollection* Outputs()
	{
		return &outputs;
	}

	ExecutionState Status() const
	{
		return status;
	}

	std::string Name() const
	{
		return name;
	}
	void SetName(std::string name)
	{
		this->name = name;
	}

	bool LogEnabled() const
	{
		return logEnabled;
	}
	void SetLogEnabled(bool value)
	{
		logEnabled = value;
	}



	virtual ~Element()
	{
	}



	virtual void Execute()
	{
		if (status != ExecutionState::WaitingForExecute)
			throw InvalidOperationException("status != ExecutionState::WaitingForExecute");

		thread.Start();

		Log("Element (%s) Execute.\n", name.c_str());
	}

	virtual void Wake()
	{
		pthread_mutex_lock(&waitMutex);

		canSleep = false;

		//pthread_cond_signal(&waitCondition);
		if (pthread_cond_broadcast(&waitCondition) != 0)
		{
			throw Exception("Element::Wake - pthread_cond_broadcast failed.");
		}

		pthread_mutex_unlock(&waitMutex);

		Log("Element (%s) Wake.\n", name.c_str());
	}

	virtual void Terminate()
	{
		if (status != ExecutionState::Executing)
			throw InvalidOperationException();

		SetExecutionState(ExecutionState::Terminating);
		Flush();

		thread.Cancel();
		thread.Join();

		Log("Element (%s) Terminate.\n", name.c_str());
	}


	virtual void ChangeState(MediaState oldState, MediaState newState)
	{
		// TODO: Allow to abort change?
		state = newState;

		Wake();

		Log("Element (%s) ChangeState oldState=%d newState=%d.\n", name.c_str(), (int)oldState, (int)newState);
	}

	virtual MediaState State()
	{
		return state;
	}
	virtual void SetState(MediaState value)
	{
		if (state != value)
		{
			//MediaState oldState = state;
			//state = value;

			ChangeState(state, value);
		}
	}

	void WaitForExecutionState(ExecutionState state)
	{
		pthread_mutex_lock(&executionWaitMutex);
		
		while (status != state)
		{
			pthread_cond_wait(&executionWaitCondition, &executionWaitMutex);
		}
		
		pthread_mutex_unlock(&executionWaitMutex);

	}


	// DEBUG
	void Log(const char* message, ...)
	{
		if (logEnabled)
		{
			struct timeval tp;
			gettimeofday(&tp, NULL);
			double ms = tp.tv_sec + tp.tv_usec * 0.0001;

			char text[1024];
			sprintf(text, "[%s : %f] %s", Name().c_str(), ms, message);


			va_list argptr;
			va_start(argptr, message);
			vfprintf(stderr, text, argptr);
			va_end(argptr);
		}
	}
};


typedef std::shared_ptr<Element> ElementSPTR;
typedef std::weak_ptr<Element> ElementWPTR;