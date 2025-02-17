// Copyright (C) 2018-2020 - DevSH Graphics Programming Sp. z O.O.
// This file is part of the "Nabla Engine".
// For conditions of distribution and use, see copyright notice in nabla.h

//
#include "../common/BasicMultiQueueApplication.hpp"

namespace nbl::examples
{

// We inherit from an application that tries to find Graphics and Compute queues
// because applications with presentable images often want to perform Graphics family operations
// Virtual Inheritance because apps might end up doing diamond inheritance
class SingleNonResizableWindowApplication : public virtual BasicMultiQueueApplication
{
		using base_t = BasicMultiQueueApplication;

	public:
		using base_t::base_t;

		virtual bool onAppInitialized(smart_refctd_ptr<ISystem>&& system) override
		{
		#ifdef _NBL_PLATFORM_WINDOWS_
			m_winMgr = nbl::ui::IWindowManagerWin32::create();
		#else
			#error "Unimplemented!"
		#endif

			m_window = nullptr;
		}

		virtual bool keepRunning() override
		{
			if (!m_window)
				return false;

			if (getWindowCallback(i)->windowGotClosed())
			{
				// unlike many other frameworks you can delete and create windows mid-execution independently of each other
				m_window[i] = nullptr;
				continue;
			}
		}

	protected:
		virtual ui::IWindow::E_CREATE_FLAGS getWindowCreationFlags() const
		{
			return IWindow::ECF_NONE;
		}

		core::smart_refctd_ptr<ui::IWindowManager> m_winMgr;
		core::smart_refctd_ptr<ui::IWindow> m_window;
};
}


using namespace nbl;
using namespace core;
using namespace system;
using namespace ui;

class HelloSwapchainApp final : public examples::SingleNonResizableWindowApplication
{
		using base_t = examples::SingleNonResizableWindowApplication;
		using clock_t = std::chrono::steady_clock;

	public:
		using base_t::base_t;

		bool onAppInitialized(smart_refctd_ptr<ISystem>&& system) override
		{
			// Remember to call the base class initialization!
			if (!base_t::onAppInitialized(std::move(system)))
				return false;
			
			// Help the CI a bit by providing a timeout option
			// TODO: @Hazardu maybe we should make a unified argument parser/handler for all examples?
			if (base_t::argv.size()>=3 && argv[1]=="-timeout_seconds")
				timeout = std::chrono::seconds(std::atoi(argv[2].c_str()));

			// not showing mouse capture yet
			for (auto i=0; i<WindowCount; i++)
			{
				IWindow::SCreationParams params = {};
				params.callback = make_smart_refctd_ptr<WindowEventDemoCallback>(m_logger.get());
				params.width = 256;
				params.height = 256;
				params.x = (16+params.width)*i;
				params.y = 300;
				params.flags = windowFlags[i];
				params.windowCaption = "Test Window "+std::to_string(i);
				windows[i] = winManager->createWindow(std::move(params));
			}

			start = clock_t::now();
			return true;
		}

		// We do a very simple thing, and just keep on clearing the swapchain image to red and present
		void workLoopBody() override
		{
		}

		//
		bool keepRunning() override
		{
			if (duration_cast<decltype(timeout)>(clock_t::now()-start)>timeout)
				return false;

			return static_cast<IWindowClosedCallback*>(m_window->getEventCallback());
		}

		//
		bool onAppTerminated() override
		{
			return base_t::onAppTerminated();
		}

	protected:
		// Just like in the HelloUI app we add a timeout
		std::chrono::seconds timeout = std::chrono::seconds(0x7fffFFFFu);
		clock_t::time_point start;
};

// define an entry point as always!
NBL_MAIN_FUNC(HelloSwapchainApp)

#if 0
// Before we get onto creating a window, we need to discuss how Nabla handles input, clipboards and cursor control
// Unfortunately most Operating Systems reporting Keyboards, Mice and even Gamepad input to windows as opposed to plain peripherals.
// While it makes sense for security of clipboard data, its quite dubious for Keyboard input which gets events even when window is not in focus.
// This means we need the window object to get any input, clipboard, or cursor control, and that Nabla is a 39GB Keylogger SDK.
class WindowEventDemoCallback : public nbl::ui::IWindow::IEventCallback
{
	public:
		// Just by the way, the default loggers are all thread safe
		WindowEventDemoCallback(ILogger* logger) : m_logger(smart_refctd_ptr<ILogger>(logger)), m_gotWindowClosedMsg(false) {}

		// unless you create a separate callback per window, both will "trip" this condition
		bool windowGotClosed() const {return m_gotWindowClosedMsg;}

		// Now we get to discuss a real innovation of Nabla, double-buffered and timestamped input by default.
		// Also removable input devices, try running the sample and connect/disconnect your USB keyboard or mouse.
		// The way this works is that each device has an associated "channel" which is a double circular buffer.
		// Why double you ask? To be thread safe, as the window worker thread constantly pushes inputs events and
		// their timestamps into the Back Buffer as soon as it receives them (constantly polls or sleeps waiting for input).
		// While the consumer thread is in charge of "draining" the Back Buffer into the Front Buffer fast enough to avoid overflows.
		// This approach is great because you don't need to pepper your rendering code with input polling calls
		// if you want late-latch input polling for low latency rendering techniques such as reprojection.
		// All your events sit sorted in a buffer of increating timestamps ready to be consumed multiple times.

		// The utility `ChannelType::CChannelConsumer<F>` takes a functional which requires two methods defined
		template <class ChannelType>
		struct SimpleEventProcessor
		{
			inline void operator()(ChannelType::range_t eventRange, const ChannelType* channel)
			{
				if (!eventRange.empty())
					m_logger.log("%d new events in channel %p since last processing!",ILogger::ELL_PERFORMANCE,eventRange.size(),channel);
			}

			// If you want to trigger this, break the application in debug mode click around A LOT and resume (or just introduce a sleep I guess)
			inline void overflow(const size_t unconsumedEventCount, const ChannelType* channel)
			{
				m_logger.log(
					"Detected overflow, %d unconsumed events in channel %p of size %d!",ILogger::ELL_ERROR,
					unconsumedEventCount,channel,channel->getFrontBufferCapacity()
				);
			}

			// The `opt` stands for optional, its just a wrapper that nicely drops any method calls
			// if the pointer is null so we don't have to check for logger being valid.
			// This is not a smart pointer, but thats fine because its `WindowEventDemoCallback`'s logger
			nbl::system::logger_opt_ptr m_logger;
		};

		// Why is the consumer a separate thing from the channel? To allow multiple unrelated agents to consume events independently!
		template<class ChannelType> requires std::is_base_of_v<nbl::ui::IInputEventChannel,ChannelType>
		using consumer_t = typename ChannelType::template CChannelConsumer<SimpleEventProcessor<ChannelType>>;
		
		// They need to be reference counted, because the buffers need to stay alive for as long as someone is using them
		template<class ChannelType>
		using consumers_t = nbl::core::vector<smart_refctd_ptr<consumer_t<ChannelType>>>;
		auto getMouseEventConsumers() {return m_mice.getAll();}
		auto getKeyboardEventConsumers() {return m_keyboards.getAll();}

	private:
		// Because we'll add/remove channels to our container from a separate thread and ask in another, we need to mutex all the operations on it, hence the wrapping.
		template <class ChannelType>
		class Channels final
		{
			public:
				void add(smart_refctd_ptr<ChannelType>&& chan, logger_opt_ptr logger)
				{
					std::unique_lock lock(contLock);
					consumers.push_back(make_smart_refctd_ptr<consumer_t<ChannelType>>(SimpleEventProcessor<ChannelType>{logger},std::move(chan)));
				}
				void remove(const ChannelType* const chan)
				{
					std::unique_lock lock(contLock);
					consumers.erase(
						std::find_if(
							consumers.begin(), consumers.end(),
							[chan](const auto& consumer)->bool {return consumer->getChannel()==chan; }
						)
					);
				}

				// return a copy of the container of channels, the individual channels are thread-safe to use
				consumers_t<ChannelType> getAll()
				{
					std::unique_lock lock(contLock);
					return consumers;
				}

			private:
				core::mutex contLock;
				consumers_t<ChannelType> consumers;
		};
		
		// TODO: documentation and discussion, we first need to change the signature of these since some assumptions were mismatched
		// for now lets say they all need to return true if they return bool
		bool onWindowShown_impl() override 
		{
			m_logger.log("Window Shown", ILogger::ELL_INFO);
			return true;
		}
		bool onWindowHidden_impl() override
		{
			m_logger.log("Window hidden", ILogger::ELL_INFO);
			return true;
		}
		// Using `ILogger::ELL_PERFORMANCE` because it always prints in all configs (doesn't get filtered out).
		bool onWindowMoved_impl(int32_t x, int32_t y) override
		{
			m_logger.log("Window window moved to { %d, %d }", ILogger::ELL_DEBUG, x, y);
			return true;
		}
		bool onWindowResized_impl(uint32_t w, uint32_t h) override
		{
			m_logger.log("Window resized to { %u, %u }", ILogger::ELL_DEBUG, w, h);
			return true;
		}
		bool onWindowMinimized_impl() override
		{
			m_logger.log("Window minimized", ILogger::ELL_INFO);
			return true;
		}
		bool onWindowMaximized_impl() override
		{
			m_logger.log("Window maximized", ILogger::ELL_INFO);
			return true;
		}
		void onGainedMouseFocus_impl() override
		{
			m_logger.log("Window gained mouse focus", ILogger::ELL_INFO);
		}
		void onLostMouseFocus_impl() override
		{
			m_logger.log("Window lost mouse focus", ILogger::ELL_INFO);
		}
		void onGainedKeyboardFocus_impl() override
		{
			m_logger.log("Window gained keyboard focus", ILogger::ELL_INFO);
		}
		void onLostKeyboardFocus_impl() override
		{
			m_logger.log("Window lost keyboard focus", ILogger::ELL_INFO);
		}
		bool onWindowClosed_impl() override
		{
			m_logger.log("Window closed", ILogger::ELL_INFO);
			m_gotWindowClosedMsg = true;
			return true;
		}
		
		void onMouseConnected_impl(smart_refctd_ptr<nbl::ui::IMouseEventChannel>&& mch) override
		{
			m_logger.log("A mouse %p has been connected", ILogger::ELL_INFO, mch);
			m_mice.add(std::move(mch),m_logger.get().get());
		}
		void onMouseDisconnected_impl(IMouseEventChannel* mch) override
		{
			m_logger.log("A mouse %p has been disconnected", ILogger::ELL_INFO, mch);
			m_mice.remove(mch);
		}
		void onKeyboardConnected_impl(smart_refctd_ptr<nbl::ui::IKeyboardEventChannel>&& kbch) override
		{
			m_logger.log("A keyboard %p has been connected", ILogger::ELL_INFO, kbch);
			m_keyboards.add(std::move(kbch),m_logger.get().get());
		}
		void onKeyboardDisconnected_impl(IKeyboardEventChannel* kbch) override
		{
			m_logger.log("A keyboard %p has been disconnected", ILogger::ELL_INFO, kbch);
			m_keyboards.remove(kbch);
		}

		Channels<IMouseEventChannel> m_mice;
		Channels<IKeyboardEventChannel> m_keyboards;
		nbl::system::logger_opt_smart_ptr m_logger;
		bool m_gotWindowClosedMsg;
};
#endif