#include "sierrachart.h"
#include <vector>
#include <string>
#include <algorithm>
#include <cctype>

// Name the custom study DLL
SCDLLName("Footprint Imbalance Trading Study")

// Drawing helper to draw highlight or marker at a specific price level
void DrawImbalanceHighlight(SCStudyInterfaceRef sc, int BarIndex, double Price, COLORREF Color, int LineNumber, int HighlightStyle)
{
	s_UseTool Tool;
	Tool.Clear();
	Tool.ChartNumber = sc.ChartNumber;
	Tool.LineNumber = LineNumber;
	Tool.AddMethod = UTAM_ADD_OR_ADJUST;

	if (HighlightStyle == 0) // Rectangle Highlight
	{
		Tool.DrawingType = DRAWING_RECTANGLEHIGHLIGHT;
		Tool.BeginIndex = BarIndex;
		Tool.EndIndex = BarIndex;
		Tool.BeginValue = static_cast<t_ChartArrayDataType>(Price);
		Tool.EndValue = static_cast<t_ChartArrayDataType>(Price + sc.TickSize); // Height is 1 tick

		Tool.Color = Color;             // Outline color
		Tool.SecondaryColor = Color;     // Fill color
		Tool.TransparencyLevel = 75;    // Semi-transparent
		Tool.LineWidth = 1;
		Tool.ExtendLeft = 0;            // Explicitly disable horizontal extension
		Tool.ExtendRight = 0;           // Explicitly disable horizontal extension
	}
	else // Triangle Marker (Foreground)
	{
		Tool.DrawingType = DRAWING_MARKER;
		Tool.BeginIndex = BarIndex;
		// Position marker slightly above the price level for buy, below for sell to avoid covering text
		if (Color == RGB(0, 180, 0)) // Buy Imbalance (Green)
		{
			Tool.BeginValue = static_cast<t_ChartArrayDataType>(Price + sc.TickSize * 0.2);
			Tool.MarkerType = MARKER_TRIANGLEUP;
		}
		else // Sell Imbalance (Red)
		{
			Tool.BeginValue = static_cast<t_ChartArrayDataType>(Price + sc.TickSize * 0.8);
			Tool.MarkerType = MARKER_TRIANGLEDOWN;
		}

		Tool.Color = Color;             // Outline color
		Tool.SecondaryColor = Color;     // Fill color (solid foreground)
		Tool.TransparencyLevel = 0;     // Opaque so it shows on top of solid backgrounds
		Tool.MarkerSize = 8;
	}

	sc.UseTool(Tool);
}

SCSFExport scsf_FootprintImbalanceTrading(SCStudyInterfaceRef sc)
{
	// Section 1 - Configuration and Defaults
	if (sc.SetDefaults)
	{
		sc.GraphName = "Footprint Imbalance & Stacked Trading";
		sc.StudyDescription = "Detects diagonal footprint bid/ask volume imbalances, highlights them, and executes trades with dynamic sizing based on asset class.";
		sc.GraphRegion = 0; // Draw on the main chart
		sc.AutoLoop = 1;    // Automatic looping enabled

		// Configure Subgraphs for signal visualization
		sc.Subgraph[0].Name = "Stacked Buy Imbalance Signal";
		sc.Subgraph[0].DrawStyle = DRAWSTYLE_POINT;
		sc.Subgraph[0].PrimaryColor = RGB(0, 255, 0);
		sc.Subgraph[0].LineWidth = 4;
		sc.Subgraph[0].DrawZeros = 0; // Prevent plotting 0.0 values from squishing the chart scale

		sc.Subgraph[1].Name = "Stacked Sell Imbalance Signal";
		sc.Subgraph[1].DrawStyle = DRAWSTYLE_POINT;
		sc.Subgraph[1].PrimaryColor = RGB(255, 0, 0);
		sc.Subgraph[1].LineWidth = 4;
		sc.Subgraph[1].DrawZeros = 0; // Prevent plotting 0.0 values from squishing the chart scale

		// Configurable inputs
		sc.Input[0].Name = "Imbalance Ratio (e.g. 3.0 = 300%)";
		sc.Input[0].SetFloat(2.5f);

		sc.Input[1].Name = "Static Min Volume (if Dynamic is Off)";
		sc.Input[1].SetInt(10);

		sc.Input[2].Name = "Consecutive Levels for Stacked Imbalance";
		sc.Input[2].SetInt(3);

		sc.Input[3].Name = "Enable Automated Trading";
		sc.Input[3].SetYesNo(false);

		sc.Input[4].Name = "Static Trade Qty (if Dynamic is Off)";
		sc.Input[4].SetInt(1);

		sc.Input[5].Name = "Stop Loss (in ticks, 0 to disable)";
		sc.Input[5].SetInt(20);

		sc.Input[6].Name = "Profit Target (in ticks, 0 to disable)";
		sc.Input[6].SetInt(40);

		sc.Input[7].Name = "Alert Sound Number (0 to disable)";
		sc.Input[7].SetAlertSoundNumber(0);

		sc.Input[8].Name = "Drawing Base Line Number Offset";
		sc.Input[8].SetInt(10000);

		sc.Input[9].Name = "Max History Days to Process";
		sc.Input[9].SetInt(5);

		// Dynamic scaling inputs
		sc.Input[10].Name = "Use Dynamic Volume Threshold";
		sc.Input[10].SetYesNo(true);

		sc.Input[11].Name = "Volume Threshold Multiplier";
		sc.Input[11].SetFloat(1.0f);

		sc.Input[12].Name = "Use Dynamic Position Sizing";
		sc.Input[12].SetYesNo(true);

		sc.Input[13].Name = "Base Order Quantity";
		sc.Input[13].SetInt(1);

		// NEW: Highlight style selection to support solid background overlays
		sc.Input[14].Name = "Highlight Style (0=Rectangle, 1=Triangle)";
		sc.Input[14].SetInt(1); // Default to 1 (Triangle Marker) for foreground overlay visibility

		// Raise this on higher timeframes / wide-range bars where a bar can span
		// more than 500 footprint price levels (the old fixed cap left stale
		// highlights undeleted once a bar exceeded it).
		sc.Input[15].Name = "Max Drawing Offsets Per Bar (raise for higher TF / wide-range bars)";
		sc.Input[15].SetInt(2000);

		// Trading configuration flags
		sc.AllowMultipleEntriesInSameDirection = false;
		sc.AllowOnlyOneTradePerBar = true;
		sc.SupportAttachedOrdersForTrading = true;
		sc.SendOrdersToTradeService = false; // Safe default; overridden by "Enable Automated Trading" each bar below
		sc.MaintainTradeStatisticsAndTradesData = true;

		return;
	}

	// Section 2 - Data Processing

	// Verify that Volume-At-Price (VAP) data is available
	if (static_cast<int>(sc.VolumeAtPriceForBars->GetNumberOfBars()) < sc.ArraySize)
		return;

	// Fetch inputs
	float ImbalanceRatio = sc.Input[0].GetFloat();
	int StaticMinVolume = sc.Input[1].GetInt();
	int StackedCount = sc.Input[2].GetInt();
	bool EnableTrading = sc.Input[3].GetYesNo();
	int StaticQuantity = sc.Input[4].GetInt();
	int StopTicks = sc.Input[5].GetInt();
	int TargetTicks = sc.Input[6].GetInt();
	int AlertSoundNum = sc.Input[7].GetAlertSoundNumber();
	int BaseLineNum = sc.Input[8].GetInt();
	int MaxDays = sc.Input[9].GetInt();

	bool UseDynamicThreshold = sc.Input[10].GetYesNo();
	float VolumeMultiplier = sc.Input[11].GetFloat();
	bool UseDynamicSizing = sc.Input[12].GetYesNo();
	int BaseQuantity = sc.Input[13].GetInt();
	int HighlightStyle = sc.Input[14].GetInt();
	int MaxDrawingOffsets = sc.Input[15].GetInt();

	// "Enable Automated Trading" now directly controls live order routing —
	// previously SendOrdersToTradeService was hardcoded false, so orders
	// never actually reached the trade service regardless of this input.
	sc.SendOrdersToTradeService = EnableTrading;

	// Avoid recalculating very old historical bars to preserve performance
	SCDateTimeMS StartDateTime = sc.BaseDateTimeIn[sc.ArraySize - 1];
	StartDateTime.SubtractDays(MaxDays);
	if (sc.BaseDateTimeIn[sc.Index] < StartDateTime)
		return;

	int BarIndex = sc.Index;

	// Calculate Dynamic Minimum Volume Threshold based on historical average volume per price level
	int MinVolume = StaticMinVolume;
	if (UseDynamicThreshold)
	{
		double total_level_volume = 0;
		int count = 0;
		int start_idx = (BarIndex >= 100) ? (BarIndex - 100) : 0;

		for (int i = start_idx; i < BarIndex; ++i)
		{
			int levels = sc.VolumeAtPriceForBars->GetSizeAtBarIndex(i);
			if (levels > 0)
			{
				total_level_volume += (sc.Volume[i] / static_cast<double>(levels));
				count++;
			}
		}

		if (count > 0)
		{
			double avg_volume_per_level = total_level_volume / count;
			MinVolume = static_cast<int>(avg_volume_per_level * VolumeMultiplier);
			if (MinVolume < 1) MinVolume = 1; // Safety boundary
		}
		else
		{
			MinVolume = StaticMinVolume; // Fallback
		}
	}

	// Calculate Dynamic Position Sizing (Contract Quantity)
	int TradeQuantity = StaticQuantity;
	if (UseDynamicSizing)
	{
		std::string Symbol(sc.Symbol.GetChars());
		std::transform(Symbol.begin(), Symbol.end(), Symbol.begin(), ::toupper);

		// Check if micro contract (MES, MNQ, MGC, MCL, SIL, MYM).
		// Note: CME's micro silver symbol is "SIL", not "MSI" — there is no "MSI" contract.
		bool is_micro = (Symbol.find("MES") != std::string::npos ||
		                 Symbol.find("MNQ") != std::string::npos ||
		                 Symbol.find("MGC") != std::string::npos ||
		                 Symbol.find("MCL") != std::string::npos ||
		                 Symbol.find("SIL") != std::string::npos ||
		                 Symbol.find("MYM") != std::string::npos);

		int asset_multiplier = 1;
		if (Symbol.find("GC") != std::string::npos)       // Gold (GC, MGC)
		{
			asset_multiplier = 2;
		}
		else if (Symbol.find("SI") != std::string::npos)  // Silver (SI, SIL)
		{
			asset_multiplier = 3;
		}
		else if (Symbol.find("CL") != std::string::npos)  // Crude Oil (CL, MCL)
		{
			asset_multiplier = 2;
		}
		else if (Symbol.find("ES") != std::string::npos)  // S&P 500 (ES, MES)
		{
			asset_multiplier = 1;
		}
		else if (Symbol.find("NQ") != std::string::npos)  // Nasdaq (NQ, MNQ)
		{
			asset_multiplier = 1;
		}
		else if (Symbol.find("YM") != std::string::npos)  // Dow Jones (YM, MYM)
		{
			asset_multiplier = 1;
		}

		TradeQuantity = BaseQuantity * asset_multiplier;

		// Scale up by 10x for micro contracts to match risk profile of standard contracts
		if (is_micro)
		{
			TradeQuantity *= 10;
		}
	}

	// Explicitly clear all potential drawings for this bar from previous calculations
	for (int Offset = 0; Offset < MaxDrawingOffsets; ++Offset)
	{
		sc.DeleteUserDrawnACSDrawing(sc.ChartNumber, BaseLineNum + (BarIndex * MaxDrawingOffsets) + Offset);
	}

	int PriceLevels = sc.VolumeAtPriceForBars->GetSizeAtBarIndex(BarIndex);
	if (PriceLevels <= 0)
		return;

	// Reset signal subgraphs
	sc.Subgraph[0][BarIndex] = 0.0f;
	sc.Subgraph[1][BarIndex] = 0.0f;

	// Keep track of which price levels have imbalances inside this bar
	std::vector<bool> buy_imbalances(PriceLevels, false);
	std::vector<bool> sell_imbalances(PriceLevels, false);

	// Pass 1: Detect individual imbalances and draw highlights
	for (int PriceIndex = 0; PriceIndex < PriceLevels; ++PriceIndex)
	{
		s_VolumeAtPriceV2 *p_Curr = nullptr;
		if (!sc.VolumeAtPriceForBars->GetVAPElementAtIndex(BarIndex, PriceIndex, &p_Curr))
			continue;

		double Price = p_Curr->PriceInTicks * sc.TickSize;
		int LineNum = BaseLineNum + (BarIndex * MaxDrawingOffsets) + PriceIndex;

		// 1. Check buying imbalance: Ask volume at current price vs Bid volume at price below
		if (PriceIndex > 0)
		{
			s_VolumeAtPriceV2 *p_Prev = nullptr;
			if (sc.VolumeAtPriceForBars->GetVAPElementAtIndex(BarIndex, PriceIndex - 1, &p_Prev))
			{
				if (p_Curr->PriceInTicks == p_Prev->PriceInTicks + 1)
				{
					double ask_vol = p_Curr->AskVolume;
					double bid_vol = p_Prev->BidVolume;

					if (ask_vol >= bid_vol * ImbalanceRatio && ask_vol >= MinVolume)
					{
						buy_imbalances[PriceIndex] = true;
						DrawImbalanceHighlight(sc, BarIndex, Price, RGB(0, 180, 0), LineNum, HighlightStyle);
					}
				}
			}
		}

		// 2. Check selling imbalance: Bid volume at current price vs Ask volume at price above
		if (PriceIndex < PriceLevels - 1)
		{
			s_VolumeAtPriceV2 *p_Next = nullptr;
			if (sc.VolumeAtPriceForBars->GetVAPElementAtIndex(BarIndex, PriceIndex + 1, &p_Next))
			{
				if (p_Next->PriceInTicks == p_Curr->PriceInTicks + 1)
				{
					double bid_vol = p_Curr->BidVolume;
					double ask_vol = p_Next->AskVolume;

					if (bid_vol >= ask_vol * ImbalanceRatio && bid_vol >= MinVolume)
					{
						sell_imbalances[PriceIndex] = true;
						DrawImbalanceHighlight(sc, BarIndex, Price, RGB(180, 0, 0), LineNum, HighlightStyle);
					}
				}
			}
		}
	}

	// Pass 2: Check for stacked imbalances (consecutive price levels)
	int consecutive_buy_count = 0;
	int consecutive_sell_count = 0;
	bool stacked_buy_found = false;
	bool stacked_sell_found = false;
	double stacked_buy_price = 0.0;
	double stacked_sell_price = 0.0;

	for (int PriceIndex = 0; PriceIndex < PriceLevels; ++PriceIndex)
	{
		s_VolumeAtPriceV2 *p_Curr = nullptr;
		sc.VolumeAtPriceForBars->GetVAPElementAtIndex(BarIndex, PriceIndex, &p_Curr);
		double Price = p_Curr->PriceInTicks * sc.TickSize;

		if (buy_imbalances[PriceIndex])
		{
			consecutive_buy_count++;
			if (consecutive_buy_count >= StackedCount)
			{
				stacked_buy_found = true;
				stacked_buy_price = Price; // Use the top price level of the stack
			}
		}
		else
		{
			consecutive_buy_count = 0;
		}

		if (sell_imbalances[PriceIndex])
		{
			consecutive_sell_count++;
			if (consecutive_sell_count >= StackedCount)
			{
				stacked_sell_found = true;
				stacked_sell_price = Price; // Use the bottom price level of the stack
			}
		}
		else
		{
			consecutive_sell_count = 0;
		}
	}

	// Update study subgraphs for visualization (only if found)
	if (stacked_buy_found)
	{
		sc.Subgraph[0][BarIndex] = static_cast<float>(stacked_buy_price);
	}
	if (stacked_sell_found)
	{
		sc.Subgraph[1][BarIndex] = static_cast<float>(stacked_sell_price);
	}

	// Section 3 - Automated Order Placement
	// Only trade on the real-time active bar
	if (BarIndex == sc.ArraySize - 1)
	{
		// Both directions stacked on the same bar is an ambiguous read (e.g. a wide
		// bar sweeping both extremes) — skip entries/exits rather than silently
		// favoring buy over sell.
		bool conflicting_signal = stacked_buy_found && stacked_sell_found;

		s_SCPositionData Position;
		sc.GetTradePosition(Position);

		if (Position.PositionQuantity != 0)
		{
			// Signal-based exit: flatten if an opposing stacked imbalance appears
			// while in a position, instead of relying solely on the stop/target bracket.
			bool exit_long = (Position.PositionQuantity > 0) && stacked_sell_found;
			bool exit_short = (Position.PositionQuantity < 0) && stacked_buy_found;

			if (!conflicting_signal && (exit_long || exit_short))
			{
				if (EnableTrading)
				{
					sc.FlattenPosition();
				}

				if (AlertSoundNum > 0)
				{
					SCString Msg;
					Msg.Format("Opposing stacked imbalance — flattening position (was Qty: %d)!", Position.PositionQuantity);
					sc.SetAlert(AlertSoundNum - 1, Msg);
				}
			}
		}
		else
		{
			// Use persistent state variables to ensure we only place one trade per bar
			int& LastTradedBar = sc.GetPersistentInt(0);

			if (LastTradedBar != BarIndex && !conflicting_signal)
			{
				bool order_placed = false;
				s_SCNewOrder NewOrder;
				NewOrder.OrderQuantity = TradeQuantity;
				NewOrder.OrderType = SCT_ORDERTYPE_MARKET;
				NewOrder.TimeInForce = SCT_TIF_GOOD_TILL_CANCELED;
				NewOrder.TextTag = "stacked_imbalance";

				// Define bracket offsets
				if (StopTicks > 0)
				{
					NewOrder.Stop1Offset = StopTicks * sc.TickSize;
					NewOrder.AttachedOrderStop1Type = SCT_ORDERTYPE_STOP;
				}
				if (TargetTicks > 0)
				{
					NewOrder.Target1Offset = TargetTicks * sc.TickSize;
					NewOrder.AttachedOrderTarget1Type = SCT_ORDERTYPE_LIMIT;
				}

				if (stacked_buy_found)
				{
					if (EnableTrading)
					{
						sc.BuyEntry(NewOrder);
					}

					if (AlertSoundNum > 0)
					{
						SCString Msg;
						Msg.Format("Stacked Buying Imbalance Signal Triggered at price %s (Qty: %d)!", sc.FormatGraphValue(stacked_buy_price, sc.ValueFormat).GetChars(), TradeQuantity);
						sc.SetAlert(AlertSoundNum - 1, Msg);
					}
					order_placed = true;
				}
				else if (stacked_sell_found)
				{
					if (EnableTrading)
					{
						sc.SellEntry(NewOrder);
					}

					if (AlertSoundNum > 0)
					{
						SCString Msg;
						Msg.Format("Stacked Selling Imbalance Signal Triggered at price %s (Qty: %d)!", sc.FormatGraphValue(stacked_sell_price, sc.ValueFormat).GetChars(), TradeQuantity);
						sc.SetAlert(AlertSoundNum - 1, Msg);
					}
					order_placed = true;
				}

				if (order_placed)
				{
					LastTradedBar = BarIndex; // Flag that trade was executed on this bar
				}
			}
		}
	}
}
