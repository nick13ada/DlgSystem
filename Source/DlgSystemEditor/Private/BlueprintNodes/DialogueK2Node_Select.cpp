// Copyright 2017-2018 Csaba Molnar, Daniel Butum
#include "DialogueK2Node_Select.h"

#include "EdGraphUtilities.h"
#include "KismetCompiler.h"
#include "BlueprintNodeSpawner.h"
#include "BlueprintActionDatabaseRegistrar.h"
#include "Kismet/KismetMathLibrary.h"
#include "Kismet/KismetSystemLibrary.h"

#include "DlgSystemEditorPrivatePCH.h"
#include "DlgDialogue.h"
#include "DlgManager.h"

#define LOCTEXT_NAMESPACE "DlgK2Node_Select"

const FString UDialogueK2Node_Select::PIN_VariableName(TEXT("VariableName"));
const FString UDialogueK2Node_Select::PIN_DefaultValue(TEXT("DefaultValue"));

//////////////////////////////////////////////////////////////////////////
// FKCHandler_Select
class FKCHandler_Select : public FNodeHandlingFunctor
{
protected:
	// Mutiple nodes possible? isn't this called only on this node
	TMap<UEdGraphNode*, FBPTerminal*> BoolTermMap;

public:
	FKCHandler_Select(FKismetCompilerContext& InCompilerContext) : FNodeHandlingFunctor(InCompilerContext)
	{
	}

	void RegisterNets(FKismetFunctionContext& Context, UEdGraphNode* Node) override
	{
		FNodeHandlingFunctor::RegisterNets(Context, Node);
		const UDialogueK2Node_Select* SelectNode = CastChecked<UDialogueK2Node_Select>(Node);

		// Create the net for the return value manually as it's a special case Output Direction pin
		{
			UEdGraphPin* ReturnPin = SelectNode->GetReturnValuePin();
			FBPTerminal* Term = Context.CreateLocalTerminalFromPinAutoChooseScope(ReturnPin, Context.NetNameMap->MakeValidName(ReturnPin));
			Context.NetMap.Add(ReturnPin, Term);
		}

		// Create a terminal to determine if the compare was successful or not
		FBPTerminal* BoolTerm = Context.CreateLocalTerminal();
		BoolTerm->Type.PinCategory = UEdGraphSchema_K2::PC_Boolean;
		BoolTerm->Source = Node;
		BoolTerm->Name = Context.NetNameMap->MakeValidName(Node) + TEXT("_CmpSuccess");
		BoolTermMap.Add(Node, BoolTerm);
	}

	void Compile(FKismetFunctionContext& Context, UEdGraphNode* Node) override
	{
		/*
		 * Pseudocode of how this is compiled to:
		 * We have N option pins - Options[N]
		 *
		 * IndexValue = ConditionTerm
		 * ReturnValue = ReturnTerm
		 * PrevIfNotStatement = null
		 *
		 * for Option in Options:
		 *    OptionValue = Value of Option
		 *    CallConditionFunctionStatement = AddStatement `BoolTerm = ConditionFunction(IndexValue, OptionValue)`
		 *
		 *    // where the previous statement jumps if it fails
		 *    if PrevIfNotStatement is not null:
		 *       PrevIfNotStatement.JumpTarget = CallConditionFunctionStatement
		 *
		 *    // the target is set above
		 *    IfNotStatement = AddStatement `GoToTargetIfNot(BoolTerm, JumpTarget=null)`
		 *
		 *    // Add return option for this Option
		 *    AddStatement `ReturnValue = OptionValue`
		 *
		 *   PrevIfNotStatement = IfNotStatement
		 *   // add some goto statements that allows us to to safely exit the loop
		 *
		 * // point goto statements to a noop at the end
		 */

		// Cast the node and get all the input pins, the options we are selecting from
		UDialogueK2Node_Select* SelectNode = CastChecked<UDialogueK2Node_Select>(Node);
		const TArray<UEdGraphPin*> OptionPins = SelectNode->GetOptionPins();

		// Get the kismet term for the (Condition or Index) that will determine which option to use
		const UEdGraphPin* VariableNameConditionPin = FEdGraphUtilities::GetNetFromPin(SelectNode->GetVariableNamePin());
		FBPTerminal** ConditionTerm = Context.NetMap.Find(VariableNameConditionPin);

		// Get the kismet term for the return value
		const UEdGraphPin* ReturnPin = SelectNode->GetReturnValuePin();
		FBPTerminal** ReturnTerm = Context.NetMap.Find(ReturnPin);

		// Get the kismet term for the default value
		const UEdGraphPin* DefaultPin = FEdGraphUtilities::GetNetFromPin(SelectNode->GetDefaultValuePin());
		FBPTerminal** DefaultTerm = Context.NetMap.Find(DefaultPin);

		// Don't proceed if there is no return value or there is no selection
		if (ConditionTerm == nullptr || ReturnTerm == nullptr || DefaultTerm == nullptr)
		{
			return;
		}

		// Get the function that determines how the condition is computed
		UFunction* ConditionFunction = SelectNode->GetConditionalFunction();

		// Find the local boolean for use in the equality call function below (BoolTerm = result of EqualEqual_NameName)
		// Aka the result of the ConditionFunction
		FBPTerminal* BoolTerm = BoolTermMap.FindRef(SelectNode);

		// We need to keep a pointer to the previous IfNot statement so it can be linked to the next conditional statement
		FBlueprintCompiledStatement* PrevIfNotStatement = nullptr;

		// Keep an array of all the unconditional goto statements so we can clean up their jumps after the noop statement is created
		TArray<FBlueprintCompiledStatement*> GotoStatementList;

		// Loop through all the options
		const int32 OptionsNum = OptionPins.Num();
		for (int32 OptionIndex = 0; OptionIndex < OptionsNum; OptionIndex++)
		{
			UEdGraphPin* OptionPin = OptionPins[OptionIndex];

			// Create a CallFunction statement with the condition function from the Select Node class
			// The Previous option (PrevIfNotStatement) points to this CallConditionFunctionStatement
			{
				// This is our Condition function.
				FBlueprintCompiledStatement& CallConditionFunctionStatement = Context.AppendStatementForNode(Node);
				CallConditionFunctionStatement.Type = KCST_CallFunction;
				CallConditionFunctionStatement.FunctionToCall = ConditionFunction;
				CallConditionFunctionStatement.FunctionContext = nullptr;
				CallConditionFunctionStatement.bIsParentContext = false;

				// BoolTerm will be the return value of the condition statement
				CallConditionFunctionStatement.LHS = BoolTerm;

				// Compare index value == option value
				// The condition passed into the Select node
				CallConditionFunctionStatement.RHS.Add(*ConditionTerm);

				// Create a literal/constant for the current option pin
				FBPTerminal* LiteralTerm = Context.CreateLocalTerminal(ETerminalSpecification::TS_Literal);
				LiteralTerm->bIsLiteral = true;

				// Does the name of the input pin matches the VariableName Pin value?
				LiteralTerm->Type.PinCategory = UEdGraphSchema_K2::PC_Name;
				LiteralTerm->Name = OptionPin->PinName;

				// Compare against the current literal
				CallConditionFunctionStatement.RHS.Add(LiteralTerm);

				// If there is a previous IfNot statement, hook this one to that one for jumping
				if (PrevIfNotStatement)
				{
					CallConditionFunctionStatement.bIsJumpTarget = true;
					PrevIfNotStatement->TargetLabel = &CallConditionFunctionStatement;
				}
			}

			// Create a GotoIfNot statement using the BoolTerm from above as the condition
			FBlueprintCompiledStatement* IfNotStatement = &Context.AppendStatementForNode(Node);
			IfNotStatement->Type = KCST_GotoIfNot;
			IfNotStatement->LHS = BoolTerm;

			// Create an assignment statement
			// If the option matches, make the return (terminal) be the value of our option
			{
				FBlueprintCompiledStatement& AssignStatement = Context.AppendStatementForNode(Node);
				AssignStatement.Type = KCST_Assignment;
				AssignStatement.LHS = *ReturnTerm;

				// Get the kismet terminal from the option pin
				UEdGraphPin* OptionPinToTry = FEdGraphUtilities::GetNetFromPin(OptionPin);
				FBPTerminal** OptionTerm = Context.NetMap.Find(OptionPinToTry);
				if (!OptionTerm)
				{
					Context.MessageLog.Error(*LOCTEXT("Error_UnregisterOptionPin", "Unregister option pin @@").ToString(), OptionPin);
					return;
				}
				AssignStatement.RHS.Add(*OptionTerm);
			}

			// Create an unconditional goto to exit the node
			FBlueprintCompiledStatement& GotoStatement = Context.AppendStatementForNode(Node);
			GotoStatement.Type = KCST_UnconditionalGoto;
			GotoStatementList.Add(&GotoStatement);

			// If this is the last IfNot statement, hook the next jump target to be the default value
			// as all the options are exhausted
			if (OptionIndex == OptionsNum - 1)
			{
				FBlueprintCompiledStatement& AssignStatement = Context.AppendStatementForNode(Node);
				AssignStatement.Type = KCST_Assignment;
				AssignStatement.bIsJumpTarget = true;
				AssignStatement.LHS = *ReturnTerm;
				AssignStatement.RHS.Add(*DefaultTerm);

				// Hook the IfNot statement's jump target to this assign statement
				IfNotStatement->TargetLabel = &AssignStatement;
			}

			PrevIfNotStatement = IfNotStatement;
		}

		// Create a noop to jump to so the unconditional goto statements can exit the node after successful assignment
		FBlueprintCompiledStatement& NopStatement = Context.AppendStatementForNode(Node);
		NopStatement.Type = KCST_Nop;
		NopStatement.bIsJumpTarget = true;

		// Loop through the unconditional goto statements and fix their jump targets
		for (FBlueprintCompiledStatement* GotoStatement : GotoStatementList)
		{
			GotoStatement->TargetLabel = &NopStatement;
		}
	}
};

UDialogueK2Node_Select::UDialogueK2Node_Select(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	VariableType = EDlgVariableType::DlgVariableTypeInt;
	AdvancedPinDisplay = ENodeAdvancedPins::NoPins;
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Begin UEdGraphNode interface
void UDialogueK2Node_Select::AllocateDefaultPins()
{
	RefreshVariablePinType();
	RefreshPinNames();
	const UEdGraphSchema_K2* Schema = GetDefault<UEdGraphSchema_K2>();

	// constants for allmost all pins
	const FString PinSubCategory = TEXT("");
	UObject* PinSubCategoryObject = nullptr;
	constexpr bool bIsArray = false;
	constexpr bool bIsReference = false;
	constexpr bool bIsConst = false;

	// Create the return value
	{
		UEdGraphPin* ReturnPin = CreatePin(EGPD_Output, VariablePinType, PinSubCategory, PinSubCategoryObject,
											bIsArray, bIsReference, UEdGraphSchema_K2::PN_ReturnValue, bIsConst, INDEX_PIN_Return);
		ReturnPin->bDisplayAsMutableRef = false;
	}

	// Create the variable name pin, the one the selections are based on
	{
		UEdGraphPin* VariableNamePin = CreatePin(EGPD_Input, UEdGraphSchema_K2::PC_Name, PinSubCategory, PinSubCategoryObject,
													bIsArray, bIsReference, PIN_VariableName, bIsConst, INDEX_PIN_VariableName);
		VariableNamePin->bDisplayAsMutableRef = false;
		VariableNamePin->PinToolTip = TEXT("The Index/Condition Name that tells what option value to use.");
		Schema->SetPinAutogeneratedDefaultValueBasedOnType(VariableNamePin);
	}

	// Create the default value pin
	{
		UEdGraphPin* DefaultPin = CreatePin(EGPD_Input, VariablePinType, PinSubCategory, PinSubCategoryObject,
											bIsArray, bIsReference, PIN_DefaultValue, bIsConst, INDEX_PIN_Default);
		DefaultPin->bDisplayAsMutableRef = false;
		DefaultPin->PinToolTip = TEXT("The default value used if the Variable Name does not match any of the options above");
		Schema->SetPinAutogeneratedDefaultValueBasedOnType(DefaultPin);
	}

	// Create the option pins at the end of the array
	for (const FName& PinName : PinNames)
	{
		const FString PinNameString = PinName.ToString();
		UEdGraphPin* NewPin = CreatePin(EGPD_Input, VariablePinType, PinSubCategory, PinSubCategoryObject,
										bIsArray, bIsReference, PinNameString);
		NewPin->bDisplayAsMutableRef = false;
		Schema->SetPinAutogeneratedDefaultValueBasedOnType(NewPin);
	}

	Super::AllocateDefaultPins();
}

FText UDialogueK2Node_Select::GetTooltipText() const
{
	return LOCTEXT("DlgSelectNodeTooltipInt", "Return the int variable based on the name");
}

FText UDialogueK2Node_Select::GetNodeTitle(ENodeTitleType::Type TitleType) const
{
	return LOCTEXT("DlgSelectInt", "Select Dialogue Int");
}

FSlateIcon UDialogueK2Node_Select::GetIconAndTint(FLinearColor& OutColor) const
{
	static FSlateIcon Icon(FEditorStyle::GetStyleSetName(), "GraphEditor.Select_16x");
	return Icon;
}
// End UEdGraphNode interface
////////////////////////////////////////////////////////////////////////////////////////////////////////////////

////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Begin UK2Node Interface
FNodeHandlingFunctor* UDialogueK2Node_Select::CreateNodeHandler(FKismetCompilerContext& CompilerContext) const
{
	return static_cast<FNodeHandlingFunctor*>(new FKCHandler_Select(CompilerContext));
}

bool UDialogueK2Node_Select::IsConnectionDisallowed(const UEdGraphPin* MyPin, const UEdGraphPin* OtherPin, FString& OutReason) const
{
	if (OtherPin && (OtherPin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec))
	{
		OutReason = LOCTEXT("ExecConnectionDisallowed", "Cannot connect with Exec pin.").ToString();
		return true;
	}

	return Super::IsConnectionDisallowed(MyPin, OtherPin, OutReason);
}

void UDialogueK2Node_Select::GetMenuActions(FBlueprintActionDatabaseRegistrar& ActionRegistrar) const
{
	// actions get registered under specific object-keys; the idea is that
	// actions might have to be updated (or deleted) if their object-key is
	// mutated (or removed)... here we use the node's class (so if the node
	// type disappears, then the action should go with it)
	UClass* ActionKey = GetClass();

	// to keep from needlessly instantiating a UBlueprintNodeSpawner, first
	// check to make sure that the registrar is looking for actions of this type
	// (could be regenerating actions for a specific asset, and therefore the
	// registrar would only accept actions corresponding to that asset)
	if (ActionRegistrar.IsOpenForRegistration(ActionKey))
	{
		UBlueprintNodeSpawner* NodeSpawner = UBlueprintNodeSpawner::Create(GetClass());
		check(NodeSpawner);

		ActionRegistrar.AddBlueprintAction(ActionKey, NodeSpawner);
	}
}

FText UDialogueK2Node_Select::GetMenuCategory() const
{
	return LOCTEXT("DlgGetMenuCategory", "Dialogue Select");
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Begin own functions
UFunction* UDialogueK2Node_Select::GetConditionalFunction()
{
	// The IndexPin (select by type)  is always an String (FName), so only use that
	const FName FunctionName = GET_FUNCTION_NAME_CHECKED(UKismetMathLibrary, EqualEqual_NameName);
	return FindField<UFunction>(UKismetMathLibrary::StaticClass(), FunctionName);
}

void UDialogueK2Node_Select::GetPrintStringFunction(FName& FunctionName, UClass** FunctionClass)
{
	FunctionName = GET_FUNCTION_NAME_CHECKED(UKismetSystemLibrary, PrintWarning);
	*FunctionClass = UKismetSystemLibrary::StaticClass();
}

bool UDialogueK2Node_Select::RefreshPinNames()
{
	const FName ParticipantName = FDlgSystemEditorModule::GetParticipantNameFromNode(this);
	if (ParticipantName == NAME_None)
		return false;

	TArray<FName> NewPinNames;
	switch (VariableType)
	{
		case EDlgVariableType::DlgVariableTypeFloat:
			UDlgManager::GetAllDialoguesFloatNames(ParticipantName, NewPinNames);
			break;
		case EDlgVariableType::DlgVariableTypeInt:
			UDlgManager::GetAllDialoguesIntNames(ParticipantName, NewPinNames);
			break;
		default:
			unimplemented();
	}

	// Size changed, simply copy
	if (NewPinNames.Num() != PinNames.Num())
	{
		PinNames = NewPinNames;
		return true;
	}

	// Find any difference, if any
	for (int32 i = 0; i < NewPinNames.Num(); ++i)
	{
		if (NewPinNames[i] != PinNames[i])
		{
			PinNames = NewPinNames;
			return true;
		}
	}

	return false;
}
// End own functions
////////////////////////////////////////////////////////////////////////////////////////////////////////////////

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// UDlgK2Node_SelectFloat
// float variant
UDialogueK2Node_SelectFloat::UDialogueK2Node_SelectFloat(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	VariableType = EDlgVariableType::DlgVariableTypeFloat;
}

FText UDialogueK2Node_SelectFloat::GetTooltipText() const
{
	return LOCTEXT("DlgSelectNodeTooltipFloat", "Return the float variable based on the name");
}

FText UDialogueK2Node_SelectFloat::GetNodeTitle(ENodeTitleType::Type TitleType) const
{
	return LOCTEXT("DlgSelectFloat", "Select Dialogue Float");
}

#undef LOCTEXT_NAMESPACE