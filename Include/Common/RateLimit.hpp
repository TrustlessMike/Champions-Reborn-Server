#pragma once

#include <chrono>

// A budget that fills at a steady rate up to a ceiling and is spent by whoever asks.
// It starts full. Spending may take it below zero (a request is paid for after it has
// been let in); nothing more is let in until it is back above what the next one needs.
struct TokenBucket
{
	double m_tokens = 0.0;
	bool m_started = false;
	std::chrono::steady_clock::time_point m_last{};

	void Refill( std::chrono::steady_clock::time_point now, double perSecond, double ceiling )
	{
		if( !m_started )
		{
			m_started = true;
			m_tokens = ceiling;
			m_last = now;
			return;
		}

		const double seconds = std::chrono::duration< double >( now - m_last ).count();

		if( seconds > 0.0 )
		{
			m_tokens += seconds * perSecond;
			m_last = now;
		}

		if( m_tokens > ceiling )
		{
			m_tokens = ceiling;
		}
	}

	bool Has( double amount ) const
	{
		return m_tokens >= amount;
	}

	void Take( double amount )
	{
		m_tokens -= amount;
	}

	// How long until `amount` is there, at the given rate.
	std::chrono::milliseconds WaitFor( double amount, double perSecond ) const
	{
		if( m_tokens >= amount || perSecond <= 0.0 )
		{
			return std::chrono::milliseconds( 0 );
		}

		const double seconds = ( amount - m_tokens ) / perSecond;
		const double capped = seconds > 3600.0 ? 3600.0 : seconds;

		return std::chrono::milliseconds( static_cast< long long >( capped * 1000.0 ) + 1 );
	}
};
